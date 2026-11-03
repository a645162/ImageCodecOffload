// File      : tiff_codec.cpp
// Brief     : TIFF 编解码实现（GPU 预测器 + CPU LZW + 容器）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#include "tiff_codec.h"

#include <algorithm>
#include <cstring>

#include "lzw_codec.h"
#include "tiff_container.h"
#include "tiff_predict_vk.h"

namespace image_codec {
namespace vulkan {
namespace tiff {

namespace {

// 让 rows_per_strip 整除高度：strip 等长时解码端不需要为最后一条特判，
// 也让「一批 strip 就是一段连续行」这个前提一直成立。
int clampRowsPerStrip(int height, int rows_per_strip) {
  if (rows_per_strip <= 0) rows_per_strip = 64;
  rows_per_strip = std::min(rows_per_strip, height);
  if (height % rows_per_strip == 0) return rows_per_strip;
  for (int r = rows_per_strip; r >= 1; --r) {
    if (height % r == 0) return r;
  }
  return 1;
}

// 把一个 tile 的有效像素拷进已清零的 padded 块（无效区域保持 0）。
void fillPaddedTile(const ImageBuffer &img, int tile_size, int tx, int ty,
                    uint8_t *dst) {
  const size_t tile_row = static_cast<size_t>(tile_size) * img.channels;
  const int x0 = tx * tile_size;
  const int y0 = ty * tile_size;
  const int w = std::min(tile_size, img.width - x0);
  const int h = std::min(tile_size, img.height - y0);
  for (int r = 0; r < h; ++r) {
    std::memcpy(dst + static_cast<size_t>(r) * tile_row,
                img.data.data() + (static_cast<size_t>(y0 + r) * img.stride() +
                                   static_cast<size_t>(x0) * img.channels),
                static_cast<size_t>(w) * img.channels);
  }
}

// 整幅 padded tile 平面（仅 compression = none 时需要：未压缩的 tile 必须
// 是完整块，缺的补 0）。
std::vector<uint8_t> buildPaddedPlane(const ImageBuffer &img, int tile_size) {
  const int tiles_x = (img.width + tile_size - 1) / tile_size;
  const int tiles_y = (img.height + tile_size - 1) / tile_size;
  const size_t tile_bytes =
      static_cast<size_t>(tile_size) * tile_size * img.channels;
  std::vector<uint8_t> plane(tile_bytes * static_cast<size_t>(tiles_x) *
                                 static_cast<size_t>(tiles_y),
                             0);
  for (int ty = 0; ty < tiles_y; ++ty) {
    for (int tx = 0; tx < tiles_x; ++tx) {
      const size_t index = static_cast<size_t>(ty) * tiles_x + tx;
      fillPaddedTile(img, tile_size, tx, ty, plane.data() + index * tile_bytes);
    }
  }
  return plane;
}

} // namespace

bool encodeTiff(const ImageBuffer &img, const TiffEncodeOptions &options,
                std::vector<uint8_t> *out, bool *gpu_used, std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "TIFF: null output";
    return false;
  }
  out->clear();
  if (gpu_used != nullptr) *gpu_used = false;
  if (!img.valid()) {
    if (err != nullptr) *err = "TIFF: invalid image buffer";
    return false;
  }
  if (options.compression != 1 && options.compression != 5) {
    if (err != nullptr) *err = "TIFF: unsupported compression";
    return false;
  }

  const uint16_t compression =
      options.compression == 5 ? kCompressionLzw : kCompressionNone;
  // 未压缩时写 Predictor=2 是合法的，但没有任何收益（还多一个 tag），
  // 因此只在 LZW 路径启用。
  const bool predictor_active = options.predictor && options.compression == 5;

  bool gpu = false;
  PredictorGpu gpu_predictor;
  bool gpu_ready = false;
  if (predictor_active && options.use_gpu) {
    std::string why;
    if (gpu_predictor.init(options.memory_limit, options.device_index, &why)) {
      gpu_ready = true;
    } else if (!options.allow_cpu_fallback) {
      if (err != nullptr) *err = why;
      return false;
    }
    // 允许回退时不写 err：产物逐字节相同，只是 gpu_used 报 false。
  }

  // 对一批行做预测（可逆变换）。返回 false 表示「不该回退且 GPU 失败」。
  auto applyPredictor = [&](std::vector<uint8_t> &buffer, size_t row_bytes,
                            int rows) -> bool {
    if (!predictor_active || rows <= 0) return true;
    if (gpu_ready) {
      std::string why;
      if (gpu_predictor.run(buffer.data(), row_bytes, img.channels, rows,
                            options.band_rows, &why)) {
        gpu = true;
        return true;
      }
      if (!options.allow_cpu_fallback) {
        if (err != nullptr) *err = why;
        return false;
      }
      // 中途失败时 buffer 可能已经被改了一半，不能在上面再算一遍 CPU 预测。
      // 后续批次也不再尝试 GPU：每批都失败一次只会白烧时间。
      gpu_ready = false;
    }
    predictRowsCpuInPlace(buffer.data(), row_bytes, img.channels, rows);
    return true;
  };

  std::vector<uint32_t> sizes;
  std::vector<uint8_t> data;

  // ---- tiled 布局 ----
  if (options.tile_size > 0) {
    const int ts = options.tile_size;
    if (ts % 16 != 0) {
      if (err != nullptr) *err = "TIFF: tile_size must be a multiple of 16";
      return false;
    }
    const int tiles_x = (img.width + ts - 1) / ts;
    const int tiles_y = (img.height + ts - 1) / ts;
    const size_t n_tiles =
        static_cast<size_t>(tiles_x) * static_cast<size_t>(tiles_y);
    const size_t tile_row = static_cast<size_t>(ts) * img.channels;
    const size_t tile_bytes = tile_row * static_cast<size_t>(ts);

    if (options.compression == 1) {
      std::vector<uint8_t> plane = buildPaddedPlane(img, ts);
      sizes.assign(n_tiles, static_cast<uint32_t>(tile_bytes));
      data = std::move(plane);
    } else {
      // 一批若干 tile：每个 tile 的 padded 行宽一致，因此可以当成
      // 「n × ts 个独立行」一次派发，而不是一个 tile 一次。
      const size_t band_rows =
          options.band_rows > 0 ? static_cast<size_t>(options.band_rows) : 256u;
      const size_t tiles_per_batch =
          std::max<size_t>(1u, band_rows / static_cast<size_t>(ts));

      sizes.reserve(n_tiles);
      data.reserve(tile_bytes * n_tiles);

      for (size_t t0 = 0; t0 < n_tiles; t0 += tiles_per_batch) {
        const size_t count = std::min(tiles_per_batch, n_tiles - t0);
        std::vector<uint8_t> batch(tile_bytes * count, 0);

        for (size_t k = 0; k < count; ++k) {
          const size_t index = t0 + k;
          fillPaddedTile(img, ts, static_cast<int>(index % tiles_x),
                         static_cast<int>(index / tiles_x),
                         batch.data() + k * tile_bytes);
        }

        if (!applyPredictor(batch, tile_row, static_cast<int>(count * ts))) {
          return false;
        }

        for (size_t k = 0; k < count; ++k) {
          std::vector<uint8_t> compressed;
          if (lzw::encode(batch.data() + k * tile_bytes, tile_bytes,
                          compressed) != 0) {
            if (err != nullptr) *err = "TIFF: LZW encode failed (tile)";
            return false;
          }
          sizes.push_back(static_cast<uint32_t>(compressed.size()));
          data.insert(data.end(), compressed.begin(), compressed.end());
        }
      }
    }

    TiffWriter writer;
    if (!writer.openTile(img.width, img.height, img.channels,
                         static_cast<uint32_t>(ts), static_cast<uint32_t>(ts),
                         compression, n_tiles)) {
      if (err != nullptr) *err = "TIFF: writer openTile failed";
      return false;
    }
    if (predictor_active) writer.setPredictor(2);

    size_t offset = 0;
    for (size_t i = 0; i < n_tiles; ++i) {
      if (!writer.appendTile(i, data.data() + offset, sizes[i])) {
        if (err != nullptr) *err = "TIFF: appendTile failed";
        return false;
      }
      offset += sizes[i];
    }
    *out = writer.buffer();
    if (gpu_used != nullptr) *gpu_used = gpu;
    return true;
  }

  // ---- strip 布局 ----
  const int rows_per_strip =
      clampRowsPerStrip(img.height, options.rows_per_strip);
  const size_t n_strips =
      (static_cast<size_t>(img.height) + rows_per_strip - 1) / rows_per_strip;

  if (options.compression == 1) {
    sizes.reserve(n_strips);
    for (size_t i = 0; i < n_strips; ++i) {
      const size_t y0 = i * static_cast<size_t>(rows_per_strip);
      const size_t rows =
          std::min<size_t>(static_cast<size_t>(rows_per_strip),
                           static_cast<size_t>(img.height) - y0);
      const size_t bytes = rows * img.stride();
      data.insert(data.end(), img.data.begin() + y0 * img.stride(),
                  img.data.begin() + y0 * img.stride() + bytes);
      sizes.push_back(static_cast<uint32_t>(bytes));
    }
  } else {
    // 一批若干 strip。因为 rows_per_strip 整除高度，一批就是一段连续行，
    // 不必逐 strip 拷贝。
    const size_t band_rows =
        options.band_rows > 0 ? static_cast<size_t>(options.band_rows) : 256u;
    const size_t strips_per_batch =
        std::max<size_t>(1u, band_rows / static_cast<size_t>(rows_per_strip));

    sizes.reserve(n_strips);
    data.reserve(img.bytes());

    for (size_t i0 = 0; i0 < n_strips; i0 += strips_per_batch) {
      const size_t i1 = std::min(i0 + strips_per_batch, n_strips);
      const size_t y0 = i0 * static_cast<size_t>(rows_per_strip);
      const size_t y1 = std::min(static_cast<size_t>(img.height),
                                 i1 * static_cast<size_t>(rows_per_strip));

      std::vector<uint8_t> band(img.data.begin() + y0 * img.stride(),
                                img.data.begin() + y1 * img.stride());

      if (!applyPredictor(band, img.stride(), static_cast<int>(y1 - y0))) {
        return false;
      }

      for (size_t i = i0; i < i1; ++i) {
        const size_t sy = y0 + (i - i0) * static_cast<size_t>(rows_per_strip);
        const size_t rows =
            std::min<size_t>(static_cast<size_t>(rows_per_strip),
                             static_cast<size_t>(img.height) - sy);
        std::vector<uint8_t> compressed;
        if (lzw::encode(band.data() + (sy - y0) * img.stride(),
                        rows * img.stride(), compressed) != 0) {
          if (err != nullptr) *err = "TIFF: LZW encode failed (strip)";
          return false;
        }
        sizes.push_back(static_cast<uint32_t>(compressed.size()));
        data.insert(data.end(), compressed.begin(), compressed.end());
      }
    }
  }

  TiffWriter writer;
  if (!writer.openStrip(img.width, img.height, img.channels, rows_per_strip,
                        compression, n_strips)) {
    if (err != nullptr) *err = "TIFF: writer openStrip failed";
    return false;
  }
  if (predictor_active) writer.setPredictor(2);

  size_t offset = 0;
  for (size_t i = 0; i < n_strips; ++i) {
    if (!writer.appendStrip(i, data.data() + offset, sizes[i])) {
      if (err != nullptr) *err = "TIFF: appendStrip failed";
      return false;
    }
    offset += sizes[i];
  }
  *out = writer.buffer();
  if (gpu_used != nullptr) *gpu_used = gpu;
  return true;
}

bool decodeTiff(const uint8_t *data, size_t size, ImageBuffer *out,
                std::string *err) {
  if (data == nullptr || size < 8 || out == nullptr) {
    if (err != nullptr) *err = "TIFF: empty input";
    return false;
  }
  TiffReader reader;
  if (!reader.openMemory(data, size)) {
    if (err != nullptr) *err = "TIFF: header/IFD parse failed";
    return false;
  }

  int w = 0;
  int h = 0;
  int spp = 1;
  int bps = 8;
  int photometric = 0;
  int compression = 1;
  int planar = 1;
  if (!reader.imageInfo(w, h, spp, bps, photometric, compression, planar)) {
    if (err != nullptr) *err = "TIFF: core tags missing";
    return false;
  }
  if (bps != 8 || planar != 1) {
    if (err != nullptr) *err = "TIFF: only 8-bit chunky is supported";
    return false;
  }
  if (photometric == 3) {
    if (err != nullptr) *err = "TIFF: palette images are not supported";
    return false;
  }
  const bool gray = photometric == 0 || photometric == 1;
  if (gray) {
    if (spp != 1 && spp != 2) {
      if (err != nullptr) *err = "TIFF: inconsistent samplesPerPixel for gray";
      return false;
    }
  } else if (photometric == 2) {
    if (spp != 3 && spp != 4) {
      if (err != nullptr) *err = "TIFF: inconsistent samplesPerPixel for RGB";
      return false;
    }
  } else {
    if (err != nullptr) *err = "TIFF: unsupported photometric";
    return false;
  }
  if (compression != kCompressionNone && compression != kCompressionLzw) {
    if (err != nullptr) *err = "TIFF: unsupported compression";
    return false;
  }
  const bool predictor = reader.predictor() == 2;

  if (!out->allocate(w, h, spp)) {
    if (err != nullptr) *err = "TIFF: allocation failed";
    return false;
  }
  const size_t row_bytes = out->stride();
  const size_t pixel_bytes = static_cast<size_t>(spp);

  auto decodeOne = [&](const uint8_t *raw, size_t raw_size,
                       std::vector<uint8_t> &dec) -> bool {
    if (compression == kCompressionLzw) {
      return lzw::decode(raw, raw_size, dec) == 0;
    }
    dec.assign(raw, raw + raw_size);
    return true;
  };

  if (reader.isTiled()) {
    std::vector<uint32_t> offs;
    std::vector<uint32_t> counts;
    uint32_t tw = 0;
    uint32_t th = 0;
    if (!reader.getTileInfo(offs, counts, tw, th)) {
      if (err != nullptr) *err = "TIFF: tile tags invalid";
      return false;
    }
    if (tw == 0 || th == 0) {
      if (err != nullptr) *err = "TIFF: zero tile size";
      return false;
    }
    const size_t tiles_per_row = (static_cast<size_t>(w) + tw - 1) / tw;
    const size_t tile_row_bytes = static_cast<size_t>(tw) * pixel_bytes;
    const size_t tile_bytes = tile_row_bytes * th;

    for (size_t i = 0; i < offs.size(); ++i) {
      std::vector<uint8_t> raw;
      if (!reader.loadBytes(offs[i], counts[i], raw)) {
        if (err != nullptr) *err = "TIFF: tile payload out of range";
        return false;
      }
      std::vector<uint8_t> dec;
      if (!decodeOne(raw.data(), raw.size(), dec)) {
        if (err != nullptr) *err = "TIFF: tile LZW decode failed";
        return false;
      }
      if (dec.size() < tile_bytes) {
        if (err != nullptr) *err = "TIFF: tile payload too small";
        return false;
      }
      // 预测是按 padded 的整块 tile 做的，必须整块还原后再裁有效区域。
      if (predictor) {
        unpredictRowsCpu(dec.data(), tile_row_bytes, spp,
                         static_cast<int>(tile_bytes / tile_row_bytes));
      }

      const size_t tc = i % tiles_per_row;
      const size_t tr = i / tiles_per_row;
      const size_t x0 = tc * tw;
      const size_t y0 = tr * th;
      if (y0 >= static_cast<size_t>(h) || x0 >= static_cast<size_t>(w))
        continue;

      const size_t w_valid =
          std::min(static_cast<size_t>(tw), static_cast<size_t>(w) - x0);
      const size_t h_valid =
          std::min(static_cast<size_t>(th), static_cast<size_t>(h) - y0);
      for (size_t r = 0; r < h_valid; ++r) {
        std::memcpy(out->data.data() + (y0 + r) * row_bytes + x0 * pixel_bytes,
                    dec.data() + r * tile_row_bytes, w_valid * pixel_bytes);
      }
    }
    return true;
  }

  std::vector<uint32_t> offs;
  std::vector<uint32_t> counts;
  uint32_t rps = 0;
  if (!reader.getStripInfo(offs, counts, rps)) {
    if (err != nullptr) *err = "TIFF: strip tags invalid";
    return false;
  }
  if (rps == 0) rps = 1;

  for (size_t i = 0; i < offs.size(); ++i) {
    std::vector<uint8_t> raw;
    if (!reader.loadBytes(offs[i], counts[i], raw)) {
      if (err != nullptr) *err = "TIFF: strip payload out of range";
      return false;
    }
    std::vector<uint8_t> dec;
    if (!decodeOne(raw.data(), raw.size(), dec)) {
      if (err != nullptr) *err = "TIFF: strip LZW decode failed";
      return false;
    }

    const size_t y0 = i * static_cast<size_t>(rps);
    if (y0 >= static_cast<size_t>(h)) break;
    const size_t rows =
        std::min<size_t>(static_cast<size_t>(rps), static_cast<size_t>(h) - y0);
    const size_t need = rows * row_bytes;
    if (dec.size() < need) {
      if (err != nullptr) *err = "TIFF: strip payload too small";
      return false;
    }
    if (predictor) {
      unpredictRowsCpu(dec.data(), row_bytes, spp, static_cast<int>(rows));
    }
    std::memcpy(out->data.data() + y0 * row_bytes, dec.data(), need);
  }
  return true;
}

} // namespace tiff
} // namespace vulkan
} // namespace image_codec
