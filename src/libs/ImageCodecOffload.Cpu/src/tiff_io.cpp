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
  const uint8_t* data = nullptr;
  size_t size = 0;
  toff_t pos = 0;
};

tmsize_t readerRead(thandle_t handle, void* buf, tmsize_t count) {
  auto* r = static_cast<MemReader*>(handle);
  if (r == nullptr || buf == nullptr || count <= 0) return 0;

  const size_t want = static_cast<size_t>(count);
  const size_t left = r->pos < r->size ? r->size - static_cast<size_t>(r->pos) : 0;
  const size_t n = want < left ? want : left;
  if (n == 0) return 0;

  std::memcpy(buf, r->data + r->pos, n);
  r->pos += static_cast<toff_t>(n);
  return static_cast<tmsize_t>(n);
}

toff_t readerSeek(thandle_t handle, toff_t offset, int whence) {
  auto* r = static_cast<MemReader*>(handle);
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
  auto* r = static_cast<MemReader*>(handle);
  return r == nullptr ? 0 : static_cast<toff_t>(r->size);
}

int readerMap(thandle_t, void**, toff_t*) { return 0; }
void readerUnmap(thandle_t, void*, toff_t) {}

// ---- 内存写 ----
struct MemWriter {
  std::vector<uint8_t>* out = nullptr;
  toff_t pos = 0;
};

tmsize_t writerWrite(thandle_t handle, void* buf, tmsize_t count) {
  auto* w = static_cast<MemWriter*>(handle);
  if (w == nullptr || w->out == nullptr || count <= 0) return 0;

  const size_t end = static_cast<size_t>(w->pos) + static_cast<size_t>(count);
  if (w->out->size() < end) w->out->resize(end);
  std::memcpy(w->out->data() + w->pos, buf, static_cast<size_t>(count));
  w->pos += static_cast<toff_t>(count);
  return count;
}

toff_t writerSeek(thandle_t handle, toff_t offset, int whence) {
  auto* w = static_cast<MemWriter*>(handle);
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
  auto* w = static_cast<MemWriter*>(handle);
  return (w == nullptr || w->out == nullptr) ? 0
                                             : static_cast<toff_t>(w->out->size());
}

TIFF* openReader(MemReader* reader) {
  return TIFFClientOpen("memory", "r", static_cast<thandle_t>(reader),
                        readerRead, writerWrite, readerSeek, readerClose,
                        readerSize, readerMap, readerUnmap);
}

TIFF* openWriter(MemWriter* writer) {
  return TIFFClientOpen("memory", "w", static_cast<thandle_t>(writer),
                        readerRead, writerWrite, writerSeek, writerClose,
                        writerSize, readerMap, readerUnmap);
}

struct TiffCloser {
  void operator()(TIFF* tif) const {
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
    case 8:
    case 9:
      return COMPRESSION_ADOBE_DEFLATE;
    default:
      return COMPRESSION_LZW;
  }
}

// 写 tiled 布局：tile 必须是 16 的倍数，边缘不足的块用边缘像素填充
// （与 GPU 后端的做法一致，避免不同后端产出不同的边缘行为）。
bool writeTiled(TIFF* tif, const Image& image, int tile) {
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

        const uint8_t* src_row =
            image.pixels.data() + static_cast<size_t>(sy) * image.rowBytes();
        uint8_t* dst_row = block.data() + static_cast<size_t>(y) * tile * channels;

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
bool writeStrips(TIFF* tif, const Image& image, int rows_per_strip) {
  const int rows = rows_per_strip > 0 ? rows_per_strip : image.height;
  if (TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows) != 1) return false;

  const size_t row_bytes = image.rowBytes();
  const size_t strip_bytes = row_bytes * static_cast<size_t>(rows);

  for (int y = 0; y < image.height; y += rows) {
    const int n = (y + rows <= image.height) ? rows : (image.height - y);
    const auto* begin = image.pixels.data() + static_cast<size_t>(y) * row_bytes;
    if (TIFFWriteEncodedStrip(
            tif, static_cast<tstrip_t>(y / rows), const_cast<uint8_t*>(begin),
            static_cast<tmsize_t>(row_bytes * static_cast<size_t>(n))) < 0) {
      return false;
    }
  }
  (void)strip_bytes;
  return true;
}

}  // namespace

Result<std::vector<uint8_t>> encodeTiff(const Image& image,
                                        const EncodeParams& params) {
  if (!image.valid()) {
    return fail<std::vector<uint8_t>>(Status::InvalidArgument,
                                      "tiff: invalid image");
  }
  if (image.channels != 1 && image.channels != 3 && image.channels != 4) {
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
               image.channels == 1 ? PHOTOMETRIC_MINISBLACK
                                   : PHOTOMETRIC_RGB);
  TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
               compressionTag(params.tiff_compression));
  TIFFSetField(tif.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);

  if (params.tiff_compression == 5) {
    TIFFSetField(tif.get(), TIFFTAG_PREDICTOR, 1);
  }

  const int tile = params.tiff_tile_size;
  bool ok = false;
  if (tile > 0) {
    if (tile % 16 != 0) {
      return fail<std::vector<uint8_t>>(
          Status::InvalidArgument,
          "tiff: tile size must be a multiple of 16");
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

Result<Image> decodeTiff(const uint8_t* data, size_t size) {
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

  if (width == 0 || height == 0) {
    return fail<Image>(Status::DecodeFailed, "tiff: zero-sized image");
  }

  // 走 RGBA 路径：libtiff 自己处理 strip/tile、LZW/Deflate、16-bit 与调色板。
  // 只有 photometric 明确是灰度时才输出 1 通道，绝不把 1 通道升成 3 通道。
  const size_t pixels = static_cast<size_t>(width) * height;
  std::vector<uint32_t> rgba(pixels);

  if (TIFFReadRGBAImageOriented(tif.get(), width, height, rgba.data(),
                                ORIENTATION_TOPLEFT, 0) != 1) {
    return fail<Image>(Status::DecodeFailed, "tiff: RGBA decode failed");
  }

  const bool gray = (photometric == PHOTOMETRIC_MINISBLACK ||
                     photometric == PHOTOMETRIC_MINISWHITE) &&
                    spp == 1;

  Image out;
  out.width = static_cast<int>(width);
  out.height = static_cast<int>(height);
  out.channels = gray ? 1 : 3;
  out.pixels.resize(pixels * static_cast<size_t>(out.channels));

  if (gray) {
    for (size_t i = 0; i < pixels; ++i) {
      out.pixels[i] = static_cast<uint8_t>(TIFFGetR(rgba[i]));
    }
  } else {
    for (size_t i = 0; i < pixels; ++i) {
      const size_t o = i * 3;
      out.pixels[o + 0] = static_cast<uint8_t>(TIFFGetR(rgba[i]));
      out.pixels[o + 1] = static_cast<uint8_t>(TIFFGetG(rgba[i]));
      out.pixels[o + 2] = static_cast<uint8_t>(TIFFGetB(rgba[i]));
    }
  }

  return out;
}

Result<int> probeTiffChannels(const uint8_t* data, size_t size) {
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
  uint16_t samples_per_pixel = 1;
  (void)TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
  return samples_per_pixel <= 1 ? 1 : 3;
}

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
