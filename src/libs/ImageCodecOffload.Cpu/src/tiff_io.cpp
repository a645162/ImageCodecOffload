/*
 * File: tiff_io.cpp
 * Brief: Tiff io.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// tiff_io.cpp - libtiff 的编解码基线。
//
// 之所以走 TIFFClientOpen + 内存回调，而不是写临时文件：本库的输入输出都在
// 内存里（上层是视口管线与批处理），临时文件会把一次几毫秒的编码变成一次
// 磁盘往返，并且要额外处理临时文件的清理与并发。
#include "tiff_io.h"

#include <tiffio.h>

#include <cstring>
#include <memory>
#include <string>

namespace image_codec {
namespace offload {
namespace cpu {

namespace {

// ---- 内存读 ----
struct MemReader {
  const uint8_t *data = nullptr;
  size_t size = 0;
  toff_t pos = 0;
};

tmsize_t readerRead(thandle_t handle, void *buf, tmsize_t count) {
  auto *r = static_cast<MemReader *>(handle);
  if (r == nullptr || buf == nullptr || count <= 0) return 0;

  const size_t want = static_cast<size_t>(count);
  const size_t left =
      r->pos < r->size ? r->size - static_cast<size_t>(r->pos) : 0;
  const size_t n = want < left ? want : left;
  if (n == 0) return 0;

  std::memcpy(buf, r->data + r->pos, n);
  r->pos += static_cast<toff_t>(n);
  return static_cast<tmsize_t>(n);
}

toff_t readerSeek(thandle_t handle, toff_t offset, int whence) {
  auto *r = static_cast<MemReader *>(handle);
  if (r == nullptr) return static_cast<toff_t>(-1);

  toff_t base = 0;
  if (whence == SEEK_CUR) {
    base = r->pos;
  } else if (whence == SEEK_END) {
    base = static_cast<toff_t>(r->size);
  }
  r->pos = base + offset;
  return r->pos;
}

int readerClose(thandle_t) { return 0; }

toff_t readerSize(thandle_t handle) {
  auto *r = static_cast<MemReader *>(handle);
  return r == nullptr ? 0 : static_cast<toff_t>(r->size);
}

int readerMap(thandle_t, void **, toff_t *) { return 0; }
void readerUnmap(thandle_t, void *, toff_t) {}

// ---- 内存写 ----
struct MemWriter {
  std::vector<uint8_t> *out = nullptr;
  toff_t pos = 0;
};

tmsize_t writerWrite(thandle_t handle, void *buf, tmsize_t count) {
  auto *w = static_cast<MemWriter *>(handle);
  if (w == nullptr || w->out == nullptr || count <= 0) return 0;

  const size_t end = static_cast<size_t>(w->pos) + static_cast<size_t>(count);
  if (w->out->size() < end) w->out->resize(end);
  std::memcpy(w->out->data() + w->pos, buf, static_cast<size_t>(count));
  w->pos += static_cast<toff_t>(count);
  return count;
}

toff_t writerSeek(thandle_t handle, toff_t offset, int whence) {
  auto *w = static_cast<MemWriter *>(handle);
  if (w == nullptr) return static_cast<toff_t>(-1);

  toff_t base = 0;
  if (whence == SEEK_CUR) {
    base = w->pos;
  } else if (whence == SEEK_END) {
    base = static_cast<toff_t>(w->out != nullptr ? w->out->size() : 0);
  }
  w->pos = base + offset;
  return w->pos;
}

int writerClose(thandle_t) { return 0; }

toff_t writerSize(thandle_t handle) {
  auto *w = static_cast<MemWriter *>(handle);
  return (w == nullptr || w->out == nullptr)
             ? 0
             : static_cast<toff_t>(w->out->size());
}

TIFF *openReader(MemReader *reader) {
  return TIFFClientOpen("memory", "r", static_cast<thandle_t>(reader),
                        readerRead, writerWrite, readerSeek, readerClose,
                        readerSize, readerMap, readerUnmap);
}

TIFF *openWriter(MemWriter *writer) {
  return TIFFClientOpen("memory", "w", static_cast<thandle_t>(writer),
                        readerRead, writerWrite, writerSeek, writerClose,
                        writerSize, readerMap, readerUnmap);
}

struct TiffCloser {
  void operator()(TIFF *tif) const {
    if (tif != nullptr) TIFFClose(tif);
  }
};

using TiffPtr = std::unique_ptr<TIFF, TiffCloser>;

int compressionTag(int value) {
  switch (value) {
  case 1:
    return COMPRESSION_NONE;
  case 5:
    return COMPRESSION_LZW;
  case 7:
    return COMPRESSION_JPEG;
  case 8:
  case 9:
    return COMPRESSION_ADOBE_DEFLATE;
  case 32773:
    return COMPRESSION_PACKBITS;
  default:
    return COMPRESSION_LZW;
  }
}

// 写 tiled 布局：tile 必须是 16 的倍数，边缘不足的块用边缘像素填充
// （与 GPU 后端的做法一致，避免不同后端产出不同的边缘行为）。
bool writeTiled(TIFF *tif, const Image &image, int tile) {
  if (TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile) != 1) return false;
  if (TIFFSetField(tif, TIFFTAG_TILELENGTH, tile) != 1) return false;

  const int channels = image.channels;
  const size_t tile_bytes = static_cast<size_t>(tile) * tile * channels;
  std::vector<uint8_t> block(tile_bytes);

  const int tiles_x = (image.width + tile - 1) / tile;
  const int tiles_y = (image.height + tile - 1) / tile;

  for (int ty = 0; ty < tiles_y; ++ty) {
    for (int tx = 0; tx < tiles_x; ++tx) {
      for (int y = 0; y < tile; ++y) {
        int sy = ty * tile + y;
        if (sy >= image.height) sy = image.height - 1;

        const uint8_t *src_row =
            image.pixels.data() + static_cast<size_t>(sy) * image.rowBytes();
        uint8_t *dst_row =
            block.data() + static_cast<size_t>(y) * tile * channels;

        for (int x = 0; x < tile; ++x) {
          int sx = tx * tile + x;
          if (sx >= image.width) sx = image.width - 1;

          std::memcpy(dst_row + static_cast<size_t>(x) * channels,
                      src_row + static_cast<size_t>(sx) * channels,
                      static_cast<size_t>(channels));
        }
      }

      const ttile_t index = static_cast<ttile_t>(
          static_cast<size_t>(ty) * static_cast<size_t>(tiles_x) + tx);
      if (TIFFWriteEncodedTile(tif, index, block.data(), tile_bytes) < 0) {
        return false;
      }
    }
  }
  return true;
}

// 写 strip 布局。
bool writeStrips(TIFF *tif, const Image &image, int rows_per_strip) {
  const int rows = rows_per_strip > 0 ? rows_per_strip : image.height;
  if (TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows) != 1) return false;

  const size_t row_bytes = image.rowBytes();
  const size_t strip_bytes = row_bytes * static_cast<size_t>(rows);

  for (int y = 0; y < image.height; y += rows) {
    const int n = (y + rows <= image.height) ? rows : (image.height - y);
    const auto *begin =
        image.pixels.data() + static_cast<size_t>(y) * row_bytes;
    if (TIFFWriteEncodedStrip(
            tif, static_cast<tstrip_t>(y / rows), const_cast<uint8_t *>(begin),
            static_cast<tmsize_t>(row_bytes * static_cast<size_t>(n))) < 0) {
      return false;
    }
  }
  (void)strip_bytes;
  return true;
}

// ---- 解码：8-bit chunky 样本的直读路径 ----
//
// **不要**用 TIFFReadRGBAImage 读带 Alpha 的图：libtiff 的 RGBA 接口会把
// 「非关联 Alpha」（EXTRASAMPLE_UNASSALPHA，也就是我们写出的那一种）**预乘**
// 进 RGB（r' = r*a/255）。那是渲染器想要的结果，不是编解码往返想要的结果——
// 4 通道进、4 通道出时必须逐位一致，预乘一次就永久丢掉了原样本。
// 因此 8-bit chunky（planar=1）的灰度/RGB 直接按样本读；其余（调色板、16-bit、
// 分平面）才退回 RGBA 路径，且那条路径一旦遇到 Alpha 就明确拒绝，不做预乘。
bool readRaw8Bit(TIFF *tif, int width, int height, int spp, bool tile_organized,
                 std::vector<uint8_t> *out) {
  out->assign(static_cast<size_t>(width) * static_cast<size_t>(height) *
                  static_cast<size_t>(spp),
              0);

  if (!tile_organized) {
    std::vector<uint8_t> row(static_cast<size_t>(width) *
                             static_cast<size_t>(spp));
    for (int y = 0; y < height; ++y) {
      if (TIFFReadScanline(tif, row.data(), static_cast<uint32_t>(y), 0) < 0) {
        return false;
      }
      std::memcpy(out->data() + static_cast<size_t>(y) * row.size(), row.data(),
                  row.size());
    }
    return true;
  }

  uint32_t tile_w = 0;
  uint32_t tile_h = 0;
  if (TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tile_w) != 1 ||
      TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_h) != 1 || tile_w == 0 ||
      tile_h == 0) {
    return false;
  }

  const tmsize_t tile_bytes = TIFFTileSize(tif);
  if (tile_bytes <= 0) return false;
  std::vector<uint8_t> tile(static_cast<size_t>(tile_bytes));

  for (uint32_t ty = 0; ty < static_cast<uint32_t>(height); ty += tile_h) {
    for (uint32_t tx = 0; tx < static_cast<uint32_t>(width); tx += tile_w) {
      const ttile_t index = TIFFComputeTile(tif, tx, ty, 0, 0);
      if (TIFFReadEncodedTile(tif, index, tile.data(), tile_bytes) < 0) {
        return false;
      }
      // 边缘 tile 在文件里是填满的（我们写的时候用边缘像素填充），
      // 但目标图只需要有效的那几列/几行。
      const uint32_t rows = tile_h < static_cast<uint32_t>(height) - ty
                                ? tile_h
                                : static_cast<uint32_t>(height) - ty;
      const uint32_t cols = tile_w < static_cast<uint32_t>(width) - tx
                                ? tile_w
                                : static_cast<uint32_t>(width) - tx;
      for (uint32_t r = 0; r < rows; ++r) {
        std::memcpy(out->data() + (static_cast<size_t>(ty + r) *
                                       static_cast<size_t>(width) +
                                   tx) *
                                      static_cast<size_t>(spp),
                    tile.data() + static_cast<size_t>(r) * tile_w * spp,
                    static_cast<size_t>(cols) * static_cast<size_t>(spp));
      }
    }
  }
  return true;
}

} // namespace

Result<std::vector<uint8_t>> encodeTiff(const Image &image,
                                        const EncodeParams &params) {
  if (!image.valid()) {
    return fail<std::vector<uint8_t>>(Status::InvalidArgument,
                                      "tiff: invalid image");
  }
  // 1 = 灰度、2 = 灰度+A、3 = RGB、4 = RGBA。
  if (image.channels < 1 || image.channels > 4) {
    return fail<std::vector<uint8_t>>(Status::UnsupportedFormat,
                                      "tiff: unsupported channel count");
  }

  std::vector<uint8_t> out;
  MemWriter writer;
  writer.out = &out;

  TiffPtr tif(openWriter(&writer));
  if (!tif) {
    return fail<std::vector<uint8_t>>(Status::Internal,
                                      "tiff: TIFFClientOpen(w) failed");
  }

  TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH,
               static_cast<uint32_t>(image.width));
  TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH,
               static_cast<uint32_t>(image.height));
  TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL,
               static_cast<uint16_t>(image.channels));
  TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
               image.channels <= 2 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB);
  // spp 比「颜色样本数」多出来的那个样本必须显式声明成 Alpha：
  // 少了这个 tag，spp==4 的文件在规范上会被读成 CMYK，颜色整体错位。
  if (image.channels == 2 || image.channels == 4) {
    uint16_t extra = EXTRASAMPLE_UNASSALPHA;
    TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES, 1, &extra);
  }
  TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
               compressionTag(params.tiff_compression));
  TIFFSetField(tif.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);

  // Predictor 只在**明确要求**时才写标签。
  //
  // 这里曾经无条件写 TIFFTAG_PREDICTOR=1（PREDICTOR_NONE），理由是「LZW 的
  // 规范配套」。值为 1 的标签虽然像素语义等于「不预测」，却会多出一个 12
  // 字节的 IFD 条目——于是同一张图、同一组参数，走 libtiff 的旧写法和走本库
  // 就差 12 字节，「逐字节一致」这一条永远不成立。默认不写，才是最中性的
  // 行为；要显式声明 Predictor=NONE 的调用方可以传 1。
  if (params.tiff_predictor > 0) {
    TIFFSetField(tif.get(), TIFFTAG_PREDICTOR,
                 static_cast<uint16_t>(params.tiff_predictor));
  }

  // 分辨率：只有给了 DPI 才写。ResolutionUnit 缺省时按英寸（TIFF 6.0 §8 的
  // 默认解释），显式给 3 才写厘米。写 XResolution 却不写单位，是最常见的
  // 「文件能打开但尺寸标注错」的来源。
  if (params.tiff_x_dpi > 0 || params.tiff_y_dpi > 0) {
    const int x_dpi = params.tiff_x_dpi > 0 ? params.tiff_x_dpi : 72;
    const int y_dpi = params.tiff_y_dpi > 0 ? params.tiff_y_dpi : x_dpi;
    TIFFSetField(tif.get(), TIFFTAG_XRESOLUTION, static_cast<float>(x_dpi));
    TIFFSetField(tif.get(), TIFFTAG_YRESOLUTION, static_cast<float>(y_dpi));
    const uint16_t unit =
        params.tiff_resolution_unit == 3 ? RESUNIT_CENTIMETER : RESUNIT_INCH;
    TIFFSetField(tif.get(), TIFFTAG_RESOLUTIONUNIT, unit);
  }

  // JPEG 压缩的质量只在 compression=7 时才有意义；libtiff 的 JPEG 编解码
  // 通过 JPEGQUALITY 取值（1..100）。
  if (params.tiff_compression == 7) {
    const int quality = params.tiff_jpeg_quality >= 0 ? params.tiff_jpeg_quality
                                                      : params.jpeg_quality;
    TIFFSetField(tif.get(), TIFFTAG_JPEGQUALITY,
                 quality < 1 ? 1 : (quality > 100 ? 100 : quality));
  }

  const int tile = params.tiff_tile_size;
  bool ok = false;
  if (tile > 0) {
    if (tile % 16 != 0) {
      return fail<std::vector<uint8_t>>(
          Status::InvalidArgument, "tiff: tile size must be a multiple of 16");
    }
    ok = writeTiled(tif.get(), image, tile);
  } else {
    ok = writeStrips(tif.get(), image, params.tiff_rows_per_strip);
  }

  if (!ok) {
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "tiff: write failed");
  }

  if (TIFFFlush(tif.get()) != 1) {
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "tiff: flush failed");
  }

  // TIFFClose 会把目录写回；必须在 close 之后取结果。
  tif.reset();

  return out;
}

Result<Image> decodeTiff(const uint8_t *data, size_t size) {
  if (data == nullptr || size == 0) {
    return fail<Image>(Status::InvalidArgument, "tiff: empty input");
  }

  MemReader reader;
  reader.data = data;
  reader.size = size;

  TiffPtr tif(openReader(&reader));
  if (!tif) {
    return fail<Image>(Status::DecodeFailed, "tiff: not a TIFF stream");
  }

  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t spp = 1;
  uint16_t photometric = PHOTOMETRIC_MINISBLACK;
  if (TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width) != 1 ||
      TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &height) != 1) {
    return fail<Image>(Status::DecodeFailed, "tiff: missing image dimensions");
  }
  TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp);
  TIFFGetField(tif.get(), TIFFTAG_PHOTOMETRIC, &photometric);

  // ExtraSamples 才是「多出来的样本是 Alpha」的权威说明；只有 spp 大一号
  // （3->4、1->2）而没有这个 tag 的文件也按带 Alpha 处理，否则 RGBA 图会被
  // 静默截成 RGB——Alpha 一丢，合成结果就全错了。
  uint16_t extra_count = 0;
  uint16_t *extra_types = nullptr;
  const bool has_extra_tag = TIFFGetField(tif.get(), TIFFTAG_EXTRASAMPLES,
                                          &extra_count, &extra_types) == 1 &&
                             extra_count >= 1;

  if (width == 0 || height == 0) {
    return fail<Image>(Status::DecodeFailed, "tiff: zero-sized image");
  }

  uint16_t bits = 8;
  uint16_t planar = PLANARCONFIG_CONTIG;
  uint16_t orientation = ORIENTATION_TOPLEFT;
  TIFFGetField(tif.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetField(tif.get(), TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetField(tif.get(), TIFFTAG_ORIENTATION, &orientation);

  const bool gray = (photometric == PHOTOMETRIC_MINISBLACK ||
                     photometric == PHOTOMETRIC_MINISWHITE);
  // 基础样本：灰度 1 / RGB 3。Alpha 只看「有没有多出来的样本」。
  const int base = gray ? 1 : 3;
  const bool alpha = has_extra_tag || spp > static_cast<uint16_t>(base);

  const size_t pixels = static_cast<size_t>(width) * height;

  // 8-bit chunky 直读：灰度/RGB（带不带 Alpha 都行），逐样本往返无损。
  // 只在这个形状下走；调色板、16-bit、分平面交给下面的 RGBA 路径。
  const bool raw_shape =
      bits == 8 && planar == PLANARCONFIG_CONTIG && spp >= 1 && spp <= 4 &&
      orientation == ORIENTATION_TOPLEFT &&
      (gray ? (spp == 1 || spp == 2)
            : (photometric == PHOTOMETRIC_RGB && (spp == 3 || spp == 4)));

  if (raw_shape) {
    const bool tiled = TIFFIsTiled(tif.get()) != 0;
    std::vector<uint8_t> raw;
    if (!readRaw8Bit(tif.get(), static_cast<int>(width),
                     static_cast<int>(height), static_cast<int>(spp), tiled,
                     &raw)) {
      return fail<Image>(Status::DecodeFailed, "tiff: raw sample read failed");
    }

    Image out;
    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.channels = static_cast<int>(spp);
    out.pixels = std::move(raw);

    // MinIsWhite：TIFF 里 0 表示白，与本库「0 = 黑」相反。只反颜色样本，
    // Alpha 不参与反相（否则透明度会被一起取反）。
    if (photometric == PHOTOMETRIC_MINISWHITE) {
      const size_t stride = static_cast<size_t>(out.channels);
      const size_t alpha_offset = alpha ? stride - 1 : stride;
      for (size_t i = 0; i < pixels; ++i) {
        for (size_t c = 0; c < alpha_offset; ++c) {
          uint8_t &v = out.pixels[i * stride + c];
          v = static_cast<uint8_t>(255 - v);
        }
      }
    }

    return out;
  }

  // 走到 RGBA 路径还带 Alpha 的，说明不是「8-bit chunky 灰度/RGB」这种
  // 能逐样本读的形状（例如 16-bit RGBA）。libtiff 的 RGBA 接口会预乘 Alpha，
  // 拿回来的已经不是原始样本，因此这里明确拒绝，绝不悄悄返回被改过的像素。
  if (alpha) {
    return fail<Image>(
        Status::UnsupportedFormat,
        "tiff: alpha in this layout (16-bit / planar / palette) cannot be "
        "decoded without premultiplication");
  }

  // 走 RGBA 路径：libtiff 自己处理 strip/tile、LZW/Deflate、16-bit 与调色板。
  // 这里已经没有 Alpha，RGBA 里的 A 恒为 255，不会被预乘影响。
  std::vector<uint32_t> rgba(pixels);

  if (TIFFReadRGBAImageOriented(tif.get(), width, height, rgba.data(),
                                ORIENTATION_TOPLEFT, 0) != 1) {
    return fail<Image>(Status::DecodeFailed, "tiff: RGBA decode failed");
  }

  Image out;
  out.width = static_cast<int>(width);
  out.height = static_cast<int>(height);
  out.channels = base;
  out.pixels.resize(pixels * static_cast<size_t>(out.channels));

  if (gray) {
    for (size_t i = 0; i < pixels; ++i) {
      out.pixels[i] = static_cast<uint8_t>(TIFFGetR(rgba[i]));
    }
  } else {
    for (size_t i = 0; i < pixels; ++i) {
      const size_t o = i * static_cast<size_t>(out.channels);
      out.pixels[o + 0] = static_cast<uint8_t>(TIFFGetR(rgba[i]));
      out.pixels[o + 1] = static_cast<uint8_t>(TIFFGetG(rgba[i]));
      out.pixels[o + 2] = static_cast<uint8_t>(TIFFGetB(rgba[i]));
    }
  }

  return out;
}

Result<int> probeTiffChannels(const uint8_t *data, size_t size) {
  if (data == nullptr || size < 8) {
    return fail<int>(Status::UnsupportedFormat, "tiff: too small");
  }

  MemReader reader;
  reader.data = data;
  reader.size = size;
  TiffPtr tif(openReader(&reader));
  if (!tif) {
    return fail<int>(Status::UnsupportedFormat, "tiff: TIFFClientOpen failed");
  }

  // 缺省值：TIFF 规范里 SamplesPerPixel 缺省为 1（灰度/调色板）。
  // 返回真实样本数（1..4）：上层据此判断某个后端能不能表示，返回 3 会让
  // 2/4 通道的图被误判成「三通道没问题」，进而在后面静默丢掉 Alpha。
  uint16_t samples_per_pixel = 1;
  (void)TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
  if (samples_per_pixel < 1) return 1;
  if (samples_per_pixel > 4) return 4;
  return static_cast<int>(samples_per_pixel);
}

} // namespace cpu
} // namespace offload
} // namespace image_codec
