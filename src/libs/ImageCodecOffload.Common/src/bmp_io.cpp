/*
 * File: bmp_io.cpp
 * Brief: Bmp io.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// bmp_io.cpp - 最小可用的 BMP 读写。
//
// BMP 是 little-endian、行尾 4 字节对齐、默认 bottom-up。这三点弄错任何一条，
// 读出来就是一张斜的或花屏的图 —— 而「看起来不对」在自动化测试里毫无意义，
// 所以这里的每个偏移都按 spec 直写。
#include "bmp_io.h"

#include <cstring>

namespace image_codec {
namespace offload {
namespace common {

namespace {

uint16_t readLe16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void writeLe16(std::vector<uint8_t> *out, size_t offset, uint16_t value) {
  (*out)[offset + 0] = static_cast<uint8_t>(value & 0xFF);
  (*out)[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void writeLe32(std::vector<uint8_t> *out, size_t offset, uint32_t value) {
  (*out)[offset + 0] = static_cast<uint8_t>(value & 0xFF);
  (*out)[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  (*out)[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  (*out)[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

size_t align4(size_t value) { return (value + 3u) & ~static_cast<size_t>(3u); }

// BITMAPINFOHEADER 大小：本库只写这一种（40 字节，兼容性最好）。
constexpr uint32_t kInfoHeaderSize = 40;

} // namespace

Result<BmpHeader> probeBmp(const uint8_t *data, size_t size) {
  if (data == nullptr || size < 54) {
    return fail<BmpHeader>(Status::InvalidArgument, "bmp: too small");
  }
  if (data[0] != 'B' || data[1] != 'M') {
    return fail<BmpHeader>(Status::UnsupportedFormat, "bmp: bad magic");
  }

  const uint32_t header_size = readLe32(data + 14);
  if (header_size < kInfoHeaderSize || size < 14u + header_size) {
    return fail<BmpHeader>(Status::UnsupportedFormat,
                           "bmp: unsupported header");
  }

  const int32_t width = static_cast<int32_t>(readLe32(data + 18));
  const int32_t raw_height = static_cast<int32_t>(readLe32(data + 22));
  const uint16_t bit_count = readLe16(data + 28);
  if (width <= 0 || raw_height == 0) {
    return fail<BmpHeader>(Status::InvalidArgument, "bmp: bad dimensions");
  }
  if (bit_count != 8 && bit_count != 24 && bit_count != 32) {
    return fail<BmpHeader>(Status::UnsupportedFormat,
                           "bmp: only 8/24/32-bit BMP is supported");
  }

  BmpHeader out;
  out.width = width;
  out.height = raw_height > 0 ? raw_height : -raw_height;
  // 32-bit 就是 4 通道：把 Alpha 丢掉会让「这张图是不是带透明」这个判断在
  // 探测阶段就丢失，后续再怎么努力也补不回来。
  out.channels = bit_count == 8 ? 1 : (bit_count == 24 ? 3 : 4);
  return out;
}

Result<Image> readBmp(const uint8_t *data, size_t size) {
  if (data == nullptr || size < 54) {
    return fail<Image>(Status::InvalidArgument, "bmp: too small");
  }
  if (data[0] != 'B' || data[1] != 'M') {
    return fail<Image>(Status::UnsupportedFormat, "bmp: bad magic");
  }

  const uint32_t pixel_offset = readLe32(data + 10);
  const uint32_t header_size = readLe32(data + 14);
  if (header_size < kInfoHeaderSize || size < 14u + header_size) {
    return fail<Image>(Status::UnsupportedFormat, "bmp: unsupported header");
  }

  const int32_t width = static_cast<int32_t>(readLe32(data + 18));
  const int32_t raw_height = static_cast<int32_t>(readLe32(data + 22));
  const uint16_t planes = readLe16(data + 26);
  const uint16_t bit_count = readLe16(data + 28);
  const uint32_t compression = readLe32(data + 30);

  if (width <= 0 || raw_height == 0 || planes != 1) {
    return fail<Image>(Status::InvalidArgument, "bmp: bad dimensions");
  }
  // BI_RGB（无压缩）以及 32-bit 的 BI_BITFIELDS（标准 RGBA 掩码）。
  // RLE 的行内布局与本库的用途无关；非标准掩码解释起来等于猜。
  if (compression == 3) {
    if (bit_count != 32) {
      return fail<Image>(Status::UnsupportedFormat,
                         "bmp: bit fields only supported for 32-bit");
    }
    if (size < 66 || readLe32(data + 54) != 0x00FF0000u ||
        readLe32(data + 58) != 0x0000FF00u ||
        readLe32(data + 62) != 0x000000FFu) {
      return fail<Image>(Status::UnsupportedFormat,
                         "bmp: unsupported 32-bit bit masks");
    }
  } else if (compression != 0) {
    return fail<Image>(Status::UnsupportedFormat,
                       "bmp: compressed BMP is not supported");
  }
  if (bit_count != 8 && bit_count != 24 && bit_count != 32) {
    return fail<Image>(Status::UnsupportedFormat,
                       "bmp: only 8/24/32-bit BMP is supported");
  }

  const bool bottom_up = raw_height > 0;
  const int height = bottom_up ? raw_height : -raw_height;

  const int channels = bit_count == 8 ? 1 : (bit_count == 24 ? 3 : 4);
  const size_t row_bytes =
      static_cast<size_t>(width) * static_cast<size_t>(channels);
  const size_t src_stride = align4(row_bytes);
  // BI_RGB 的 32-bit 没有 Alpha 语义（第四字节是保留位），按不透明处理；
  // 只有 BI_BITFIELDS 明确给出 A 掩码时才采信文件里的 Alpha。
  const bool has_alpha = bit_count == 32 && compression == 3;

  const size_t need = static_cast<size_t>(pixel_offset) +
                      src_stride * static_cast<size_t>(height);
  if (need > size) {
    return fail<Image>(Status::DecodeFailed, "bmp: truncated pixel data");
  }

  Image out;
  out.width = width;
  out.height = height;
  out.channels = channels;
  out.pixels.resize(row_bytes * static_cast<size_t>(height));

  const uint8_t *src = data + pixel_offset;
  const size_t dst_stride = row_bytes;

  // 逐行处理（行内是一次连续转换，不存在逐像素的嵌套循环）。
  for (int y = 0; y < height; ++y) {
    const int src_row = bottom_up ? (height - 1 - y) : y;
    const uint8_t *s = src + static_cast<size_t>(src_row) * src_stride;
    uint8_t *d = out.pixels.data() + static_cast<size_t>(y) * dst_stride;

    if (bit_count == 8) {
      std::memcpy(d, s, row_bytes);
      continue;
    }

    const size_t pixels = static_cast<size_t>(width);
    if (bit_count == 24) {
      for (size_t x = 0; x < pixels; ++x) {
        d[x * 3 + 0] = s[x * 3 + 2]; // BMP 存 BGR
        d[x * 3 + 1] = s[x * 3 + 1];
        d[x * 3 + 2] = s[x * 3 + 0];
      }
    } else {
      for (size_t x = 0; x < pixels; ++x) {
        d[x * 4 + 0] = s[x * 4 + 2]; // BMP 存 BGRA
        d[x * 4 + 1] = s[x * 4 + 1];
        d[x * 4 + 2] = s[x * 4 + 0];
        d[x * 4 + 3] = has_alpha ? s[x * 4 + 3] : 255;
      }
    }
  }

  if (!out.valid()) {
    return fail<Image>(Status::DecodeFailed, "bmp: inconsistent result");
  }
  return out;
}

Result<std::vector<uint8_t>> writeBmp(const Image &image) {
  if (!image.valid()) {
    return fail<std::vector<uint8_t>>(Status::InvalidArgument,
                                      "bmp: invalid image");
  }
  // 1 = 灰度、3 = RGB、4 = RGBA。2 通道（灰度+A）在 BMP 里没有表示法，
  // 明确拒绝而不是伪造成 32-bit。
  if (image.channels != 1 && image.channels != 3 && image.channels != 4) {
    return fail<std::vector<uint8_t>>(Status::UnsupportedFormat,
                                      "bmp: only 1/3/4-channel images");
  }

  const bool gray = image.channels == 1;
  const bool rgba = image.channels == 4;
  const uint16_t bit_count = gray ? 8 : (rgba ? 32 : 24);
  const size_t row_bytes = image.rowBytes();
  const size_t dst_stride = align4(row_bytes);
  const uint32_t palette_bytes = gray ? 256u * 4u : 0u;
  // 32-bit 必须是 BI_BITFIELDS：掩码紧跟 40 字节头之后（3 个，共 12 字节）。
  const uint32_t mask_bytes = rgba ? 12u : 0u;
  const uint32_t pixel_offset =
      14u + kInfoHeaderSize + mask_bytes + palette_bytes;

  const size_t total = static_cast<size_t>(pixel_offset) +
                       dst_stride * static_cast<size_t>(image.height);

  std::vector<uint8_t> out(total, 0);

  out[0] = 'B';
  out[1] = 'M';
  writeLe32(&out, 2, static_cast<uint32_t>(total));
  writeLe32(&out, 10, pixel_offset);

  writeLe32(&out, 14, kInfoHeaderSize);
  writeLe32(&out, 18, static_cast<uint32_t>(image.width));
  // 正高度 = bottom-up（BMP 的默认，也是兼容性最好的）。
  writeLe32(&out, 22, static_cast<uint32_t>(image.height));
  writeLe16(&out, 26, 1);
  writeLe16(&out, 28, bit_count);
  writeLe32(&out, 30, rgba ? 3u : 0u); // BI_BITFIELDS : BI_RGB
  writeLe32(
      &out, 34,
      static_cast<uint32_t>(dst_stride * static_cast<size_t>(image.height)));
  writeLe32(&out, 38, 2835); // 72 DPI
  writeLe32(&out, 42, 2835);

  if (rgba) {
    // RGBA 掩码（小端 0xAARRGGBB）：R / G / B 三个 32-bit 掩码，A 由位宽隐含。
    writeLe32(&out, 54, 0x00FF0000u);
    writeLe32(&out, 58, 0x0000FF00u);
    writeLe32(&out, 62, 0x000000FFu);
  }

  if (gray) {
    // 灰度调色板：BGRA 四元组，R = G = B = i。
    for (uint32_t i = 0; i < 256; ++i) {
      const size_t base = 14u + kInfoHeaderSize + static_cast<size_t>(i) * 4u;
      out[base + 0] = static_cast<uint8_t>(i);
      out[base + 1] = static_cast<uint8_t>(i);
      out[base + 2] = static_cast<uint8_t>(i);
      out[base + 3] = 0;
    }
  }

  const size_t pixels_per_row = static_cast<size_t>(image.width);

  for (int y = 0; y < image.height; ++y) {
    const int dst_row = image.height - 1 - y; // bottom-up 存储
    const uint8_t *s = image.pixels.data() + static_cast<size_t>(y) * row_bytes;
    uint8_t *d =
        out.data() + pixel_offset + static_cast<size_t>(dst_row) * dst_stride;

    if (gray) {
      std::memcpy(d, s, row_bytes);
    } else if (rgba) {
      for (size_t x = 0; x < pixels_per_row; ++x) {
        d[x * 4 + 0] = s[x * 4 + 2]; // RGBA -> BGRA
        d[x * 4 + 1] = s[x * 4 + 1];
        d[x * 4 + 2] = s[x * 4 + 0];
        d[x * 4 + 3] = s[x * 4 + 3];
      }
    } else {
      for (size_t x = 0; x < pixels_per_row; ++x) {
        d[x * 3 + 0] = s[x * 3 + 2]; // RGB -> BGR
        d[x * 3 + 1] = s[x * 3 + 1];
        d[x * 3 + 2] = s[x * 3 + 0];
      }
    }
  }

  return out;
}

} // namespace common
} // namespace offload
} // namespace image_codec
