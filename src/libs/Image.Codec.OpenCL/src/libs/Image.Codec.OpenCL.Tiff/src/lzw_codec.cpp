/*
 * File: lzw_codec.cpp
 * Brief: Lzw codec.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/lzw_codec.cpp - 统一编解码入口实现。
#include "lzw_codec.h"

#include "lzw_decode_cpu.h"
#include "lzw_encode_ocl.h"
#include "tiff_container.h"

#include <algorithm>
#include <cstring>

namespace image_codec {
namespace tiff {

namespace {

inline void cpuPutBits(std::vector<uint8_t> &out, uint32_t &bitbuf, int &nbits,
                       uint32_t value, int width) {
  bitbuf = (bitbuf << width) | value;
  nbits += width;
  while (nbits >= 8) {
    nbits -= 8;
    out.push_back(static_cast<uint8_t>(bitbuf >> nbits));
  }
  bitbuf &= (1u << nbits) - 1u;
}

} // namespace

int cpuLzwEncode(const uint8_t *in, size_t in_size, std::vector<uint8_t> &out) {
  out.clear();
  if (in == nullptr && in_size > 0) return -1;
  std::vector<uint32_t> dict(4096, 0);
  uint32_t bitbuf = 0;
  int nbits = 0;
  uint32_t next_code = 258;
  int width = 9;

  auto hash = [](uint32_t key) -> uint32_t {
    return ((key * 2654435761u) >> 20) & 4095u;
  };

  cpuPutBits(out, bitbuf, nbits, 256u, 9); // ClearCode
  if (in_size > 0) {
    uint32_t prefix = in[0];
    for (size_t i = 1; i < in_size; ++i) {
      const uint32_t byte = in[i];
      const uint32_t key = (prefix << 8) | byte;
      uint32_t h = hash(key);
      uint32_t e = dict[h];
      while (e != 0 && (e >> 12) != key + 1) {
        h = (h + 1) & 4095u;
        e = dict[h];
      }
      if ((e >> 12) == key + 1) {
        prefix = e & 4095u;
      } else {
        cpuPutBits(out, bitbuf, nbits, prefix, width);
        if (next_code < 4096) {
          dict[h] = ((key + 1) << 12) | next_code;
          next_code++;
          if (next_code == 512) width = 10;
          else if (next_code == 1024) width = 11;
          else if (next_code == 2048) width = 12;
        } else {
          cpuPutBits(out, bitbuf, nbits, 256u, width);
          std::fill(dict.begin(), dict.end(), 0u);
          next_code = 258;
          width = 9;
        }
        prefix = byte;
      }
    }
    cpuPutBits(out, bitbuf, nbits, prefix, width);
    // 最后一个数据码之后补一次「假想建表」的宽度记账：解码器读到最后一个码
    // 仍会建一条（下一条才用得到的）条目；若这次建表正好跨过 512/1024/2048，
    // 它会按更宽的码读 EOI，编码器不补这一步就会位流错位（缺 EOI / 多解字节）。
    if (next_code < 4096u) {
      next_code++;
      if (next_code == 512u) width = 10;
      else if (next_code == 1024u) width = 11;
      else if (next_code == 2048u) width = 12;
    }
  }
  cpuPutBits(out, bitbuf, nbits, 257u, width); // EOI
  if (nbits > 0) out.push_back(static_cast<uint8_t>(bitbuf << (8 - nbits)));
  return 0;
}

// 编码核心：把 top-down 像素打包成 TIFF 字节流（不落盘）。
// 与 tiffEncodeToFile 共用同一份实现，避免「文件路径版」和「内存版」在
// 通道/压缩参数上悄悄漂移。
int tiffEncodeToBuffer(int width, int height, int channels, const uint8_t *rgb,
                       int compression, int rows_per_strip, int backend,
                       std::vector<uint8_t> &out) {
  out.clear();
  if (rgb == nullptr || width <= 0 || height <= 0) return -1;
  // 1 = 灰度、2 = 灰度+Alpha、3 = RGB、4 = RGBA。spp 直接写进容器，
  // 因此这里必须与调用方给出的字节排布完全一致（row_bytes = width*channels）。
  if (channels < 1 || channels > 4) return -2;
  if (rows_per_strip <= 0) rows_per_strip = height;
  if (compression != 1 && compression != 5) return -2;

  // LZW（尤其 GPU）要求 strip 等长：自动调整为 height 的最大 ≤rps 因子。
  if (compression == 5 && height % rows_per_strip != 0) {
    for (int r = rows_per_strip; r >= 1; --r) {
      if (height % r == 0) {
        rows_per_strip = r;
        break;
      }
    }
  }
  const size_t n_strips =
      static_cast<size_t>((height + rows_per_strip - 1) / rows_per_strip);
  const size_t row_bytes =
      static_cast<size_t>(width) * static_cast<size_t>(channels);

  std::vector<uint32_t> sizes;
  std::vector<uint8_t> data;

  if (compression == 1) {
    sizes.reserve(n_strips);
    for (size_t i = 0; i < n_strips; ++i) {
      const size_t y0 = static_cast<size_t>(i) * rows_per_strip;
      const size_t rows = std::min<size_t>(static_cast<size_t>(rows_per_strip),
                                           static_cast<size_t>(height) - y0);
      const size_t bytes = rows * row_bytes;
      data.insert(data.end(), rgb + y0 * row_bytes,
                  rgb + y0 * row_bytes + bytes);
      sizes.push_back(static_cast<uint32_t>(bytes));
    }
  } else { // LZW
    int rc = -1;
    if (backend == 0) {
      LzwEncodeOcl enc(0);
      rc = enc.encodeStrips(rgb, width, height, channels, rows_per_strip, sizes,
                            data);
    } else {
      sizes.reserve(n_strips);
      for (size_t i = 0; i < n_strips; ++i) {
        const size_t y0 = static_cast<size_t>(i) * rows_per_strip;
        const size_t rows =
            std::min<size_t>(static_cast<size_t>(rows_per_strip),
                             static_cast<size_t>(height) - y0);
        const size_t bytes = rows * row_bytes;
        std::vector<uint8_t> comp;
        rc = cpuLzwEncode(rgb + y0 * row_bytes, bytes, comp);
        if (rc != 0) return -4;
        sizes.push_back(static_cast<uint32_t>(comp.size()));
        data.insert(data.end(), comp.begin(), comp.end());
      }
      rc = 0;
    }
    if (rc != 0) return -3;
  }

  TiffWriter w;
  if (!w.openStrip(width, height, rows_per_strip,
                   compression == 5 ? kCompressionLzw : kCompressionNone,
                   n_strips, channels)) {
    return -5;
  }
  size_t off = 0;
  for (size_t i = 0; i < n_strips; ++i) {
    if (!w.appendStrip(i, data.data() + off, sizes[i])) return -6;
    off += sizes[i];
  }
  out = w.buffer();
  if (out.empty()) return -7;
  return 0;
}

int tiffEncodeToFile(const char *path, int width, int height, int channels,
                     const uint8_t *rgb, int compression, int rows_per_strip,
                     int backend) {
  if (path == nullptr) return -1;
  std::vector<uint8_t> out;
  const int rc = tiffEncodeToBuffer(width, height, channels, rgb, compression,
                                    rows_per_strip, backend, out);
  if (rc != 0) return rc;

  FILE *f = std::fopen(path, "wb");
  if (f == nullptr) return -7;
  const size_t written = std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  return written == out.size() ? 0 : -7;
}

namespace {

// 解码核心：把已打开的 TiffReader 解成 top-down 像素。
int decodeReader(const TiffReader &r, int &width, int &height, int &channels,
                 std::vector<uint8_t> &rgb) {
  rgb.clear();
  channels = 0;

  int spp = 1, bps = 8, photometric = 0, compression = 1, planar = 1;
  if (!r.imageInfo(width, height, spp, bps, photometric, compression, planar)) {
    return -2;
  }
  // 8-bit chunky，且灰度（spp 1/2 + BlackIsZero）或 RGB（spp 3/4 + RGB）。
  // spp==2 是灰度+Alpha、spp==4 是 RGBA：这两者必须有 ExtraSamples 才能确认
  // 「多出来的那个样本是 Alpha」，否则它可能是任意数据；这里选择接受，
  // 因为 spp 与 Photometric 的组合已经唯一确定了前三个样本的语义。
  if (bps != 8 || planar != 1) return -3;
  const bool gray = (photometric == 1);
  if (!((gray && (spp == 1 || spp == 2)) ||
        (!gray && photometric == 2 && (spp == 3 || spp == 4)))) {
    return -3;
  }
  if (compression != kCompressionNone && compression != kCompressionLzw) {
    return -4;
  }
  channels = spp;

  const size_t row_bytes =
      static_cast<size_t>(width) * static_cast<size_t>(channels);
  rgb.assign(row_bytes * static_cast<size_t>(height), 0);

  auto decodeOne = [&](const std::vector<uint8_t> &raw,
                       std::vector<uint8_t> &dec) -> int {
    if (compression == kCompressionLzw) {
      return lzwDecode(raw.data(), raw.size(), dec);
    }
    dec = raw;
    return 0;
  };

  if (r.isTiled()) {
    std::vector<uint32_t> offs, counts;
    uint32_t tw = 0, th = 0;
    if (!r.getTileInfo(offs, counts, tw, th)) return -5;
    const size_t tiles_per_row = (static_cast<size_t>(width) + tw - 1) / tw;
    const size_t tile_bytes =
        static_cast<size_t>(tw) * th * static_cast<size_t>(channels);
    for (size_t i = 0; i < offs.size(); ++i) {
      std::vector<uint8_t> raw;
      if (!r.loadBytes(offs[i], counts[i], raw)) return -6;
      std::vector<uint8_t> dec;
      if (decodeOne(raw, dec) != 0) return -7;
      if (dec.size() < tile_bytes) return -8;
      const size_t tc = i % tiles_per_row;
      const size_t tr = i / tiles_per_row;
      const size_t x0 = tc * tw;
      const size_t y0 = tr * th;
      const size_t w_valid =
          std::min(static_cast<size_t>(tw), static_cast<size_t>(width) - x0);
      const size_t h_valid =
          std::min(static_cast<size_t>(th), static_cast<size_t>(height) - y0);
      for (size_t rr = 0; rr < h_valid; ++rr) {
        std::memcpy(rgb.data() + (y0 + rr) * row_bytes +
                        x0 * static_cast<size_t>(channels),
                    dec.data() + rr * static_cast<size_t>(tw) *
                                     static_cast<size_t>(channels),
                    w_valid * static_cast<size_t>(channels));
      }
    }
  } else {
    std::vector<uint32_t> offs, counts;
    uint32_t rps = 0;
    if (!r.getStripInfo(offs, counts, rps)) return -9;
    if (rps == 0) rps = 1;
    for (size_t i = 0; i < offs.size(); ++i) {
      std::vector<uint8_t> raw;
      if (!r.loadBytes(offs[i], counts[i], raw)) return -10;
      std::vector<uint8_t> dec;
      if (decodeOne(raw, dec) != 0) return -11;
      const size_t y0 = static_cast<size_t>(i) * rps;
      const size_t rows =
          std::min(static_cast<size_t>(rps), static_cast<size_t>(height) - y0);
      const size_t need = rows * row_bytes;
      const size_t copy_n = std::min(need, dec.size());
      std::memcpy(rgb.data() + y0 * row_bytes, dec.data(), copy_n);
    }
  }
  return 0;
}

} // namespace

int tiffDecodeFromFile(const char *path, int &width, int &height, int &channels,
                       std::vector<uint8_t> &pixels) {
  TiffReader r;
  if (path == nullptr || !r.openFile(path)) {
    width = height = channels = 0;
    pixels.clear();
    return -1;
  }
  return decodeReader(r, width, height, channels, pixels);
}

int tiffDecodeFromBuffer(const uint8_t *data, size_t size, int &width,
                         int &height, int &channels,
                         std::vector<uint8_t> &pixels) {
  TiffReader r;
  if (data == nullptr || size == 0 || !r.openMemory(data, size)) {
    width = height = channels = 0;
    pixels.clear();
    return -1;
  }
  return decodeReader(r, width, height, channels, pixels);
}

} // namespace tiff
} // namespace image_codec
