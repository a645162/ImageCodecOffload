/*
 * File: rgb_nv12.cpp
 * Brief: Rgb nv12.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// rgb_nv12.cpp - RGB/BGR <-> NV12 标量转换（BT.601 全范围，与 JPEG 定义一致）。
#include "rgb_nv12.h"

#include <cstddef>

namespace oic {
namespace jpeg {
namespace onevpl {

namespace {

// BT.601 全范围 RGB->YCbCr，1024 定点（>>10）。
// Y  = 0.299R + 0.587G + 0.114B
// Cb = -0.168736R - 0.331264G + 0.5B + 128
// Cr = 0.5R - 0.418688G - 0.081312B + 128
inline int rgb_to_y(int r, int g, int b) {
  return (306 * r + 601 * g + 117 * b) >> 10;
}

inline int rgb_to_cb(int r, int g, int b) {
  return ((-173 * r - 339 * g + 512 * b) >> 10) + 128;
}

inline int rgb_to_cr(int r, int g, int b) {
  return ((512 * r - 429 * g - 83 * b) >> 10) + 128;
}

// NV12(Y,Cb,Cr) -> 单个 RGB 通道，1024 定点（JPEG 全范围）。
inline void ycbcr_to_rgb(int yy, int cb, int cr, int &r, int &g, int &b) {
  const int dr = cr - 128;
  const int db = cb - 128;
  r = yy + ((1436 * dr) >> 10);
  g = yy - ((352 * db + 731 * dr) >> 10);
  b = yy + ((1816 * db) >> 10);
  if (r < 0) {
    r = 0;
  } else if (r > 255) {
    r = 255;
  }
  if (g < 0) {
    g = 0;
  } else if (g > 255) {
    g = 255;
  }
  if (b < 0) {
    b = 0;
  } else if (b > 255) {
    b = 255;
  }
}

template <typename F> void rgb_to_nv12_impl(const uint8_t *rgb, uint8_t *nv12,
                                            uint32_t w, uint32_t h, F sample) {
  // sample(pixel) -> {r,g,b}
  // 奇数的最后一行/列没有配对像素，就与自己配对（等价于复制边缘像素），
  // 这样 NV12 的色度平面在奇尺寸下也是确定值，而不是未初始化内存。
  const uint32_t chroma_w = (w + 1) / 2;
  const uint32_t chroma_h = (h + 1) / 2;
  uint8_t *Y = nv12;
  uint8_t *UV = nv12 + static_cast<std::size_t>(w) * h;
  for (uint32_t cy = 0; cy < chroma_h; ++cy) {
    const uint32_t y0 = cy * 2;
    const uint32_t y1 = y0 + 1 < h ? y0 + 1 : y0;
    for (uint32_t cx = 0; cx < chroma_w; ++cx) {
      const uint32_t x0 = cx * 2;
      const uint32_t x1 = x0 + 1 < w ? x0 + 1 : x0;
      int r0 = 0, g0 = 0, b0 = 0;
      int r1 = 0, g1 = 0, b1 = 0;
      int r2 = 0, g2 = 0, b2 = 0;
      int r3 = 0, g3 = 0, b3 = 0;
      sample(rgb, w, y0, x0, r0, g0, b0);
      sample(rgb, w, y0, x1, r1, g1, b1);
      sample(rgb, w, y1, x0, r2, g2, b2);
      sample(rgb, w, y1, x1, r3, g3, b3);
      // 2x2 chroma 平均
      const int cb = (rgb_to_cb(r0, g0, b0) + rgb_to_cb(r1, g1, b1) +
                      rgb_to_cb(r2, g2, b2) + rgb_to_cb(r3, g3, b3) + 2) >>
                     2;
      const int cr = (rgb_to_cr(r0, g0, b0) + rgb_to_cr(r1, g1, b1) +
                      rgb_to_cr(r2, g2, b2) + rgb_to_cr(r3, g3, b3) + 2) >>
                     2;
      Y[static_cast<std::size_t>(y0) * w + x0] =
          static_cast<uint8_t>(rgb_to_y(r0, g0, b0));
      if (x1 != x0) {
        Y[static_cast<std::size_t>(y0) * w + x1] =
            static_cast<uint8_t>(rgb_to_y(r1, g1, b1));
      }
      if (y1 != y0) {
        Y[static_cast<std::size_t>(y1) * w + x0] =
            static_cast<uint8_t>(rgb_to_y(r2, g2, b2));
        if (x1 != x0) {
          Y[static_cast<std::size_t>(y1) * w + x1] =
              static_cast<uint8_t>(rgb_to_y(r3, g3, b3));
        }
      }
      const std::size_t uv = static_cast<std::size_t>(cy) * w + x0;
      UV[uv] = static_cast<uint8_t>(cb);
      if (x1 != x0) {
        UV[uv + 1] = static_cast<uint8_t>(cr);
      }
    }
  }
}

} // namespace

void bgr24_to_nv12(const uint8_t *bgr, uint8_t *nv12, uint32_t w, uint32_t h) {
  rgb_to_nv12_impl(bgr, nv12, w, h,
                   [](const uint8_t *src, uint32_t width, uint32_t y,
                      uint32_t x, int &r, int &g, int &b) {
                     const uint8_t *p =
                         src + (static_cast<std::size_t>(y) * width + x) * 3;
                     b = p[0];
                     g = p[1];
                     r = p[2];
                   });
}

void rgb24_to_nv12(const uint8_t *rgb, uint8_t *nv12, uint32_t w, uint32_t h) {
  rgb_to_nv12_impl(rgb, nv12, w, h,
                   [](const uint8_t *src, uint32_t width, uint32_t y,
                      uint32_t x, int &r, int &g, int &b) {
                     const uint8_t *p =
                         src + (static_cast<std::size_t>(y) * width + x) * 3;
                     r = p[0];
                     g = p[1];
                     b = p[2];
                   });
}

void nv12_to_bgra32(const uint8_t *nv12, uint8_t *bgra, uint32_t w,
                    uint32_t h) {
  const uint8_t *Y = nv12;
  const uint8_t *UV = nv12 + static_cast<std::size_t>(w) * h;
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t *yrow = Y + static_cast<std::size_t>(y) * w;
    const uint8_t *uvrow = UV + static_cast<std::size_t>(y / 2) * w;
    uint8_t *d = bgra + static_cast<std::size_t>(y) * w * 4;
    for (uint32_t x = 0; x < w; x++) {
      const int yy = yrow[x];
      const int cb = uvrow[x & ~1u];
      const int cr = uvrow[(x & ~1u) + 1];
      int r = 0, g = 0, b = 0;
      ycbcr_to_rgb(yy, cb, cr, r, g, b);
      d[0] = static_cast<uint8_t>(b);
      d[1] = static_cast<uint8_t>(g);
      d[2] = static_cast<uint8_t>(r);
      d[3] = 255;
      d += 4;
    }
  }
}

void nv12_to_bgr24(const uint8_t *nv12, uint8_t *bgr, uint32_t w, uint32_t h) {
  const uint8_t *Y = nv12;
  const uint8_t *UV = nv12 + static_cast<std::size_t>(w) * h;
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t *yrow = Y + static_cast<std::size_t>(y) * w;
    const uint8_t *uvrow = UV + static_cast<std::size_t>(y / 2) * w;
    uint8_t *d = bgr + static_cast<std::size_t>(y) * w * 3;
    for (uint32_t x = 0; x < w; x++) {
      const int yy = yrow[x];
      const int cb = uvrow[x & ~1u];
      const int cr = uvrow[(x & ~1u) + 1];
      int r = 0, g = 0, b = 0;
      ycbcr_to_rgb(yy, cb, cr, r, g, b);
      d[0] = static_cast<uint8_t>(b);
      d[1] = static_cast<uint8_t>(g);
      d[2] = static_cast<uint8_t>(r);
      d += 3;
    }
  }
}

void nv12_to_rgb24(const uint8_t *nv12, uint8_t *rgb, uint32_t w, uint32_t h) {
  const uint8_t *Y = nv12;
  const uint8_t *UV = nv12 + static_cast<std::size_t>(w) * h;
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t *yrow = Y + static_cast<std::size_t>(y) * w;
    const uint8_t *uvrow = UV + static_cast<std::size_t>(y / 2) * w;
    uint8_t *d = rgb + static_cast<std::size_t>(y) * w * 3;
    for (uint32_t x = 0; x < w; x++) {
      const int yy = yrow[x];
      const int cb = uvrow[x & ~1u];
      const int cr = uvrow[(x & ~1u) + 1];
      int r = 0, g = 0, b = 0;
      ycbcr_to_rgb(yy, cb, cr, r, g, b);
      d[0] = static_cast<uint8_t>(r);
      d[1] = static_cast<uint8_t>(g);
      d[2] = static_cast<uint8_t>(b);
      d += 3;
    }
  }
}

} // namespace onevpl
} // namespace jpeg
} // namespace oic
