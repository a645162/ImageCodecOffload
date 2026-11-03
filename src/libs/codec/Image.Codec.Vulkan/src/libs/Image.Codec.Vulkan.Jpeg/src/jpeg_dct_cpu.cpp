// File      : jpeg_dct_cpu.cpp
// Brief     : JPEG 的纯 CPU 参考实现
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 逐像素循环在这里是刻意的例外：这是「GPU 结果的对照物」，必须在任何机器上
// 都能独立跑出结果，不需要 SIMD 也不需要设备；它的开销不影响产品路径
// （产品路径走 GPU 或父项目的 CPU 后端）。其余像素循环全部走 GPU 着色器。
#include "jpeg_dct_cpu.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "jpeg_dct_vk.h"

namespace image_codec {
namespace vulkan {
namespace jpeg {

namespace {

int clampByte(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return value;
}

// 分量平面布局：每分量 (blocks_x*8) x (blocks_y*8)，起点 plane_off。
struct PlaneView {
  const JpegImage *img = nullptr;
  const float *planes = nullptr;

  const JpegComponentInfo &comp(int c) const { return img->comp[c]; }
  int stride(int c) const { return img->comp[c].blocks_x * 8; }

  int rd(int c, int cx, int cy) const {
    const JpegComponentInfo &ci = comp(c);
    if (cx < 0) cx = 0;
    if (cx >= ci.plane_w) cx = ci.plane_w - 1;
    if (cy < 0) cy = 0;
    if (cy >= ci.plane_h) cy = ci.plane_h - 1;
    return static_cast<int>(planes[ci.plane_off + cy * stride(c) + cx]);
  }

  int sample(int c, int x, int y) const {
    const JpegComponentInfo &ci = comp(c);
    if (ci.h_samp == img->max_h && ci.v_samp == img->max_v) {
      return rd(c, x, y);
    }
    const int hratio = img->max_h / ci.h_samp;
    const int vratio = img->max_v / ci.v_samp;
    if (hratio == 2 && vratio == 2) {
      int cx = x >> 1;
      int cy = y >> 1;
      const int h = x & 1;
      const int v = y & 1;
      if (cx > ci.plane_w - 1) cx = ci.plane_w - 1;
      if (cy > ci.plane_h - 1) cy = ci.plane_h - 1;
      const int iy = cy;
      const int oy2 = (v == 0) ? (cy > 0 ? cy - 1 : cy)
                               : (cy + 1 < ci.plane_h ? cy + 1 : cy);
      const int s_cur = 3 * rd(c, cx, iy) + rd(c, cx, oy2);
      if (h == 0) {
        if (cx == 0) return (4 * s_cur + 8) >> 4;
        return (3 * s_cur + 3 * rd(c, cx - 1, iy) + rd(c, cx - 1, oy2) + 8) >>
               4;
      }
      if (cx >= ci.plane_w - 1) return (4 * s_cur + 7) >> 4;
      return (3 * s_cur + 3 * rd(c, cx + 1, iy) + rd(c, cx + 1, oy2) + 7) >> 4;
    }
    if (hratio == 2 && vratio == 1) {
      int cx = x >> 1;
      const int h = x & 1;
      if (cx > ci.plane_w - 1) cx = ci.plane_w - 1;
      const int cur = rd(c, cx, y);
      if (h == 0) {
        if (cx == 0) return cur;
        return (3 * cur + rd(c, cx - 1, y) + 1) >> 2;
      }
      if (cx >= ci.plane_w - 1) return cur;
      return (3 * cur + rd(c, cx + 1, y) + 2) >> 2;
    }
    return rd(c, x * ci.h_samp / img->max_h, y * ci.v_samp / img->max_v);
  }
};

} // namespace

bool cpuDecode(const JpegImage &img, const std::vector<float> &coeffs,
               std::vector<uint8_t> *out, std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "JPEG: null output";
    return false;
  }
  out->clear();
  if (!img.valid) {
    if (err != nullptr) *err = "JPEG: invalid image description";
    return false;
  }

  float m[64][64];
  buildDctMatrix(m);

  size_t plane_floats = 0;
  for (int c = 0; c < img.ncomp; ++c) {
    plane_floats += static_cast<size_t>(img.comp[c].blocks_x * 8) *
                    static_cast<size_t>(img.comp[c].blocks_y * 8);
  }
  std::vector<float> planes(plane_floats, 0.0f);

  for (int c = 0; c < img.ncomp; ++c) {
    const JpegComponentInfo &ci = img.comp[c];
    const int stride = ci.blocks_x * 8;
    const int blocks = ci.blocks_x * ci.blocks_y;
    for (int bid = 0; bid < blocks; ++bid) {
      const float *block = &coeffs[ci.coeff_off + bid * 64];
      const int bx = bid % ci.blocks_x;
      const int by = bid / ci.blocks_x;
      for (int p = 0; p < 64; ++p) {
        float sum = 0.0f;
        for (int k = 0; k < 64; ++k) {
          // 与着色器同序累加：m[k][p] * c[k] * q[k]。
          sum += m[k][p] * block[k] * static_cast<float>(img.quant[ci.tq].q[k]);
        }
        int iv = static_cast<int>(std::floor(sum + 128.0f + 0.5f));
        iv = clampByte(iv);
        const int px = bx * 8 + (p & 7);
        const int py = by * 8 + (p >> 3);
        planes[ci.plane_off + py * stride + px] = static_cast<float>(iv);
      }
    }
  }

  const int channels = (img.ncomp == 1) ? 1 : 3;
  out->assign(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) *
                  static_cast<size_t>(channels),
              0);

  PlaneView view;
  view.img = &img;
  view.planes = planes.data();

  for (int y = 0; y < img.height; ++y) {
    for (int x = 0; x < img.width; ++x) {
      const int yv = view.sample(0, x, y);
      if (channels == 1) {
        (*out)[static_cast<size_t>(y) * img.width + x] =
            static_cast<uint8_t>(clampByte(yv));
        continue;
      }
      const int cb = view.sample(1, x, y) - 128;
      const int cr = view.sample(2, x, y) - 128;
      const int r = clampByte(yv + ((91881 * cr + 32768) >> 16));
      const int g = clampByte(yv - ((46802 * cr + 32768) >> 16) -
                              ((22554 * cb + 32768) >> 16));
      const int b = clampByte(yv + ((116130 * cb + 32768) >> 16));
      uint8_t *o = &(*out)[(static_cast<size_t>(y) * img.width + x) * 3];
      o[0] = static_cast<uint8_t>(r);
      o[1] = static_cast<uint8_t>(g);
      o[2] = static_cast<uint8_t>(b);
    }
  }
  return true;
}

bool cpuEncode(const JpegImage &img, const uint8_t *pixels,
               std::vector<float> *coeffs, std::string *err) {
  if (coeffs == nullptr || pixels == nullptr) {
    if (err != nullptr) *err = "JPEG: null input";
    return false;
  }
  coeffs->clear();
  if (!img.valid) {
    if (err != nullptr) *err = "JPEG: invalid image description";
    return false;
  }

  float m[64][64];
  buildDctMatrix(m);

  const int in_channels = (img.ncomp == 1) ? 1 : 3;
  const int full_w = img.mcus_x * img.max_h * 8;
  const int full_h = img.mcus_y * img.max_v * 8;
  const size_t full_floats =
      static_cast<size_t>(full_w) * static_cast<size_t>(full_h);

  // 全分辨率平面 + 需要时的下采样色度平面。
  std::vector<float> y_plane(full_floats, 0.0f);
  std::vector<float> cb_plane;
  std::vector<float> cr_plane;
  if (in_channels == 3) {
    cb_plane.assign(full_floats, 0.0f);
    cr_plane.assign(full_floats, 0.0f);
  }

  for (int y = 0; y < full_h; ++y) {
    for (int x = 0; x < full_w; ++x) {
      // 块补齐区域用边缘复制，与着色器的 clamp 一致。
      const int rx = x < img.width ? x : img.width - 1;
      const int ry = y < img.height ? y : img.height - 1;
      const int src = (ry * img.width + rx) * in_channels;
      const size_t o = static_cast<size_t>(y) * full_w + x;
      if (in_channels == 1) {
        y_plane[o] = static_cast<float>(pixels[src]);
        continue;
      }
      // 与 jpeg_rgb_to_ycbcr.comp 完全相同的 16 位定点（libjpeg jccolor.c）。
      // 用整数而不是浮点，是因为浮点在 GPU 上可能被 FMA 合并成另一种累加
      // 顺序，与这里的 CPU 参考差 1 ulp；那 1 ulp 会被下游的 (a+b+c+d+2)>>2
      // 放大成 ±1 的色度差，最终变成两套不同的量化系数。
      const int r = pixels[src + 0];
      const int g = pixels[src + 1];
      const int b = pixels[src + 2];
      y_plane[o] =
          static_cast<float>((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
      cb_plane[o] = static_cast<float>(
          (-11059 * r - 21709 * g + 32768 * b + 8421376) >> 16);
      cr_plane[o] = static_cast<float>(
          (32768 * r - 27439 * g - 5329 * b + 8421376) >> 16);
    }
  }

  bool need_downsample = false;
  for (int c = 0; c < img.ncomp; ++c) {
    if (img.comp[c].h_samp != img.max_h || img.comp[c].v_samp != img.max_v) {
      need_downsample = true;
    }
  }

  std::vector<float> ds_cb;
  std::vector<float> ds_cr;
  if (need_downsample) {
    const JpegComponentInfo &ci = img.comp[1];
    const int dw = ci.blocks_x * 8;
    const int dh = ci.blocks_y * 8;
    ds_cb.assign(static_cast<size_t>(dw) * dh, 0.0f);
    ds_cr.assign(static_cast<size_t>(dw) * dh, 0.0f);

    // 抽取比例与着色器一致，从采样因子推导：4:2:2 只做水平抽取（v_ratio=1），
    // 4:2:0 才是 2x2。写死 2x2 会让 4:2:2 的下半幅色度被 clamp 到同一行。
    const int h_ratio = img.max_h / (ci.h_samp > 0 ? ci.h_samp : 1);
    const int v_ratio = img.max_v / (ci.v_samp > 0 ? ci.v_samp : 1);
    const int tap_count =
        (h_ratio < 1 ? 1 : h_ratio) * (v_ratio < 1 ? 1 : v_ratio);
    const int h_step = h_ratio < 1 ? 1 : h_ratio;
    const int v_step = v_ratio < 1 ? 1 : v_ratio;

    // (sum + count/2) / count —— h2v2 是 (a+b+c+d+2)>>2，h2v1 是 (a+b+1)>>1，
    // 同一个式子。与着色器、与 libjpeg 的盒式下采样同一条舍入；漏掉那个
    // 舍入项会让每个色度样本系统性偏低 0~1，与 GPU 侧整体错开一档。
    const auto downsample = [&](const std::vector<float> &src,
                                std::vector<float> *dst) {
      for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
          const int sx = x * h_step;
          const int sy = y * v_step;
          const auto at = [&](int px, int py) {
            const int cx = std::clamp(px, 0, full_w - 1);
            const int cy = std::clamp(py, 0, full_h - 1);
            return static_cast<int>(src[static_cast<size_t>(cy) * full_w + cx] +
                                    0.5f);
          };
          int sum = 0;
          for (int dy = 0; dy < v_step; ++dy) {
            for (int dx = 0; dx < h_step; ++dx) {
              sum += at(sx + dx, sy + dy);
            }
          }
          (*dst)[static_cast<size_t>(y) * dw + x] =
              static_cast<float>((sum + tap_count / 2) / tap_count);
        }
      }
    };
    downsample(cb_plane, &ds_cb);
    downsample(cr_plane, &ds_cr);
  }

  coeffs->assign(static_cast<size_t>(img.total_blocks) * 64, 0.0f);

  for (int c = 0; c < img.ncomp; ++c) {
    const JpegComponentInfo &ci = img.comp[c];
    const float *plane = nullptr;
    int plane_stride = full_w;
    if (c == 0) {
      plane = y_plane.data();
    } else if (c == 1) {
      plane = need_downsample ? ds_cb.data() : cb_plane.data();
      if (need_downsample) plane_stride = ci.blocks_x * 8;
    } else {
      plane = need_downsample ? ds_cr.data() : cr_plane.data();
      if (need_downsample) plane_stride = ci.blocks_x * 8;
    }

    const int blocks = ci.blocks_x * ci.blocks_y;
    for (int bid = 0; bid < blocks; ++bid) {
      const int block_x = (bid % ci.blocks_x) * 8;
      const int block_y = (bid / ci.blocks_x) * 8;
      for (int p = 0; p < 64; ++p) {
        float sum = 0.0f;
        for (int k = 0; k < 64; ++k) {
          const int lx = k & 7;
          const int ly = k >> 3;
          const float f =
              plane[static_cast<size_t>(block_y + ly) * plane_stride +
                    (block_x + lx)] -
              128.0f;
          sum += m[p][k] * f;
        }
        float qv = static_cast<float>(img.quant[ci.tq].q[p]);
        if (qv < 1.0f) qv = 1.0f;
        // 与 jpeg_fdct_quantize.comp 同一个平局偏置：精确落在 n+0.5 上的值
        // 必须两边同向取整，否则 DC（块均值）这种经常撞平局的系数会让两份
        // 实现编出不同的量化系数。
        const int qi = static_cast<int>(std::floor(sum / qv + 0.5f + 1.0e-3f));
        (*coeffs)[ci.coeff_off + bid * 64 + p] = static_cast<float>(qi);
      }
    }
  }
  return true;
}

} // namespace jpeg
} // namespace vulkan
} // namespace image_codec
