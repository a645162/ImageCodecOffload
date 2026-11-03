/*
 * File: bmp_io.cpp
 * Brief: Bmp io.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// bmp_io.cpp - BMP 读写实现（8-bit 灰度 / 24-bit RGB）。
#include "bmp_io.h"

#include <cstdio>
#include <cstring>

namespace image_codec {

namespace {

struct U32LE {
  static uint32_t get(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  }
  static void put(std::vector<uint8_t> *v, uint32_t x) {
    v->push_back(static_cast<uint8_t>(x & 0xFF));
    v->push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v->push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v->push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
  }
};

struct U16LE {
  static uint16_t get(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
  }
  static void put(std::vector<uint8_t> *v, uint16_t x) {
    v->push_back(static_cast<uint8_t>(x & 0xFF));
    v->push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
  }
};

bool read_all(const std::string &path, std::vector<uint8_t> *out) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
  uint8_t buf[65536];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out->insert(out->end(), buf, buf + n);
  }
  std::fclose(f);
  return true;
}

int row_stride_of(int width, int channels) {
  const int bits = width * channels * 8;
  return (bits + 31) / 32 * 4;
}

// 32-bit 像素区必须是 BI_BITFIELDS 且掩码就是标准 RGBA 排布，否则字节序
// 全靠猜。返回 true 表示「可以按 BGRA 逐字节解释」。
bool check_32bit_masks(const std::vector<uint8_t> &data, uint32_t dib_size) {
  if (dib_size < 40) return false;
  const uint32_t rb = U32LE::get(&data[54]);
  const uint32_t gb = U32LE::get(&data[58]);
  const uint32_t bb = U32LE::get(&data[62]);
  // 有些写入器把掩码放在 V4/V5 头的固定位置（54 字节处），另有把掩码紧跟
  // 40 字节头之后的；两者在文件里的偏移相同（54），所以只需要读一次。
  return rb == 0x00FF0000u && gb == 0x0000FF00u && bb == 0x000000FFu;
}

} // namespace

int bmp_read(const std::string &path, ImageBuffer *img, std::string *err) {
  if (img == nullptr) {
    if (err != nullptr) *err = "bmp_read: null output";
    return -1;
  }
  img->reset();

  std::vector<uint8_t> data;
  if (!read_all(path, &data)) {
    if (err != nullptr) *err = "bmp_read: cannot open " + path;
    return -1;
  }
  if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') {
    if (err != nullptr) *err = "bmp_read: not a BMP file";
    return -2;
  }

  const uint32_t data_offset = U32LE::get(&data[10]);
  const uint32_t dib_size = U32LE::get(&data[14]);
  if (dib_size < 40) {
    if (err != nullptr) *err = "bmp_read: unsupported DIB header";
    return -2;
  }

  const int32_t iw = static_cast<int32_t>(U32LE::get(&data[18]));
  const int32_t ih = static_cast<int32_t>(U32LE::get(&data[22]));
  const int width = iw < 0 ? -iw : iw;
  const int height = ih < 0 ? -ih : ih;
  const bool top_down = ih < 0;
  const uint16_t planes = U16LE::get(&data[26]);
  const uint16_t bpp = U16LE::get(&data[28]);
  const uint32_t compression = U32LE::get(&data[30]);

  // 接受的组合：
  //   8-bit / 24-bit + BI_RGB            —— 灰度与 RGB 的常规写法；
  //   32-bit + BI_BITFIELDS(标准 RGBA)  —— 带 Alpha，本库自己写出的就是这种；
  //   32-bit + BI_RGB                   —— 第四字节按规范是保留位，Alpha 补 255。
  // 其它（RLE/JPEG/PNG 内嵌/奇怪掩码）明确拒绝：解释它们等于猜。
  if (width <= 0 || height <= 0 || planes != 1) {
    if (err != nullptr) *err = "bmp_read: unsupported BMP layout";
    return -2;
  }
  if (bpp != 8 && bpp != 24 && bpp != 32) {
    if (err != nullptr) {
      *err = "bmp_read: only 8-bit grayscale / 24-bit RGB / 32-bit RGBA";
    }
    return -2;
  }
  if (bpp == 32) {
    if (compression == 3) {
      if (data.size() < 66 || !check_32bit_masks(data, dib_size)) {
        if (err != nullptr) *err = "bmp_read: unsupported 32-bit bit masks";
        return -2;
      }
    } else if (compression != 0) {
      if (err != nullptr) *err = "bmp_read: unsupported 32-bit compression";
      return -2;
    }
  } else if (compression != 0) {
    if (err != nullptr) *err = "bmp_read: unsupported BMP compression";
    return -2;
  }

  const int channels = (bpp == 8) ? 1 : (bpp == 24 ? 3 : 4);
  // BI_RGB 的 32-bit 没有 Alpha 语义，按不透明处理；BI_BITFIELDS 才采信文件值。
  const bool has_alpha = (bpp == 32) && (compression == 3);
  const int stride = row_stride_of(width, channels);
  const int row_bytes = width * channels;
  if (static_cast<uint64_t>(data_offset) +
          static_cast<uint64_t>(stride) * static_cast<uint64_t>(height) >
      data.size()) {
    if (err != nullptr) *err = "bmp_read: truncated pixel data";
    return -2;
  }

  if (!img->allocate(width, height, channels)) {
    if (err != nullptr) *err = "bmp_read: allocation failed";
    return -1;
  }

  for (int y = 0; y < height; ++y) {
    const int src_row = top_down ? y : (height - 1 - y);
    const uint8_t *src =
        data.data() + data_offset +
        static_cast<size_t>(src_row) * static_cast<size_t>(stride);
    uint8_t *dst = img->data.data() +
                   static_cast<size_t>(y) * static_cast<size_t>(row_bytes);
    if (channels == 1) {
      std::memcpy(dst, src, static_cast<size_t>(row_bytes));
      continue;
    }
    // BMP 是 BGR(A) 存储，本库统一用 RGB(A)；逐样本交换 B 与 R。
    const int bytes = channels;
    for (int x = 0; x < width; ++x) {
      const uint8_t *s = src + static_cast<size_t>(x) * bytes;
      uint8_t *d = dst + static_cast<size_t>(x) * bytes;
      if (bytes == 3) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
      } else {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = has_alpha ? s[3] : 255;
      }
    }
  }
  return 0;
}

int bmp_write_pixels(const std::string &path, const uint8_t *pixels, int width,
                     int height, int channels, std::string *err) {
  if (pixels == nullptr || width <= 0 || height <= 0 ||
      (channels != 1 && channels != 3 && channels != 4)) {
    if (err != nullptr) {
      *err = "bmp_write: invalid arguments (channels must be 1, 3 or 4)";
    }
    return -2;
  }

  const int stride = row_stride_of(width, channels);
  const uint32_t pixel_bytes =
      static_cast<uint32_t>(stride) * static_cast<uint32_t>(height);
  // 8-bit 需要 256 项调色板（每项 4 字节 BGRA），24-bit 没有调色板，
  // 32-bit 走 BI_BITFIELDS，需要在头后紧跟 3 个掩码（A 掩码由 dib 大小隐含）。
  const uint32_t palette_bytes = (channels == 1) ? 256u * 4u : 0u;
  const uint32_t mask_bytes = (channels == 4) ? 12u : 0u;
  const uint32_t data_offset = 54u + mask_bytes + palette_bytes;
  const uint32_t file_size = data_offset + pixel_bytes;

  std::vector<uint8_t> out;
  out.reserve(file_size);

  out.push_back('B');
  out.push_back('M');
  U32LE::put(&out, file_size);
  U16LE::put(&out, 0);
  U16LE::put(&out, 0);
  U32LE::put(&out, data_offset);
  U32LE::put(&out, 40); // BITMAPINFOHEADER
  U32LE::put(&out, static_cast<uint32_t>(width));
  U32LE::put(&out, static_cast<uint32_t>(height)); // 正数 = bottom-up
  U16LE::put(&out, 1);                             // planes
  U16LE::put(&out, static_cast<uint16_t>(channels * 8));
  // 32-bit 必须是 BI_BITFIELDS：BI_RGB 的第四字节按规范没有 Alpha 语义，
  // 写成 BI_RGB 会让别的程序把透明度当噪声。
  U32LE::put(&out, channels == 4 ? 3u : 0u);
  U32LE::put(&out, pixel_bytes);
  U32LE::put(&out, 2835);
  U32LE::put(&out, 2835);
  U32LE::put(&out, static_cast<uint32_t>(channels == 1 ? 256 : 0));
  U32LE::put(&out, 0);

  if (channels == 4) {
    // RGBA 掩码（小端 0xAARRGGBB）。
    U32LE::put(&out, 0x00FF0000u); // R
    U32LE::put(&out, 0x0000FF00u); // G
    U32LE::put(&out, 0x000000FFu); // B
    U32LE::put(&out, 0xFF000000u); // A
  }

  if (channels == 1) {
    // 恒等灰度调色板：索引 i 就是灰度值 i，读回来仍是 1 通道。
    for (uint32_t i = 0; i < 256; ++i) {
      out.push_back(static_cast<uint8_t>(i));
      out.push_back(static_cast<uint8_t>(i));
      out.push_back(static_cast<uint8_t>(i));
      out.push_back(0);
    }
  }

  const int row_bytes = width * channels;
  for (int y = height - 1; y >= 0; --y) {
    const uint8_t *src =
        pixels + static_cast<size_t>(y) * static_cast<size_t>(row_bytes);
    if (channels == 1) {
      out.insert(out.end(), src, src + row_bytes);
    } else {
      for (int x = 0; x < width; ++x) {
        const uint8_t *s = src + static_cast<size_t>(x) * channels;
        out.push_back(s[2]); // RGB(A) -> BGR(A)
        out.push_back(s[1]);
        out.push_back(s[0]);
        if (channels == 4) out.push_back(s[3]);
      }
    }
    for (int p = row_bytes; p < stride; ++p) {
      out.push_back(0);
    }
  }

  FILE *f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    if (err != nullptr) *err = "bmp_write: cannot write " + path;
    return -1;
  }
  const size_t written = std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  if (written != out.size()) {
    if (err != nullptr) *err = "bmp_write: short write " + path;
    return -1;
  }
  return 0;
}

int bmp_write(const std::string &path, const ImageBuffer &img,
              std::string *err) {
  if (!img.valid()) {
    if (err != nullptr) *err = "bmp_write: invalid image buffer";
    return -1;
  }
  return bmp_write_pixels(path, img.data.data(), img.width, img.height,
                          img.channels, err);
}

} // namespace image_codec
