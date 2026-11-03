/*
 * File: metrics_cpu.cpp
 * Brief: Metrics cpu.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// metrics_cpu.cpp - MSE / SSIM / MS-SSIM 的 CPU 参考实现。
//
// 存在的意义不是「CPU 更快」，而是给 GPU kernel 一个可以逐项对拍的基准：
// SSIM 的常数、边界策略、金字塔下采样方式只要有一处不同，两边的数字就会对不上，
// 而这种偏差在真实图像上往往只差 0.001——肉眼与「看起来合理」都发现不了。
//
// 因此这里的写法刻意保持「照着公式直译」：不做 SIMD、不做循环展开、不做累加
// 顺序优化。它跑得慢是设计的一部分，别在这里做性能优化。
#include <cmath>
#include <vector>

#include "metrics.h"
#include "metrics_common.h"

namespace image_codec {
namespace ocl {
namespace metrics {
namespace reference {

namespace {

// 单尺度统计（CPU）。与 kernels/metrics.cl 的 11x11 窗逐项对应。
bool scaleStats(const ImageView &a, const ImageView &b, bool with_window,
                ScaleStats *out, std::string *err) {
  if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height ||
      a.channels != b.channels) {
    if (err != nullptr) *err = "metrics: invalid image pair";
    return false;
  }

  *out = ScaleStats{};

  const int width = a.width;
  const int height = a.height;
  const int channels = a.channels;
  const size_t stride_a = a.rowBytes();
  const size_t stride_b = b.rowBytes();

  double w[kWindow];
  gaussianWeights11(w);

  double sq_sum = 0.0;
  double ssim_sum = 0.0;
  double cs_sum = 0.0;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < channels; ++c) {
        const size_t oa = static_cast<size_t>(y) * stride_a +
                          static_cast<size_t>(x * channels + c);
        const size_t ob = static_cast<size_t>(y) * stride_b +
                          static_cast<size_t>(x * channels + c);

        const double d =
            static_cast<double>(a.data[oa]) - static_cast<double>(b.data[ob]);
        sq_sum += d * d;

        if (!with_window) continue;

        double sx = 0.0;
        double sy = 0.0;
        double sxx = 0.0;
        double syy = 0.0;
        double sxy = 0.0;

        for (int dy = -kHalo; dy <= kHalo; ++dy) {
          int cy = y + dy;
          if (cy < 0) cy = 0;
          if (cy >= height) cy = height - 1;

          const double wy = w[static_cast<size_t>(dy + kHalo)];

          for (int dx = -kHalo; dx <= kHalo; ++dx) {
            int cx = x + dx;
            if (cx < 0) cx = 0;
            if (cx >= width) cx = width - 1;

            const size_t ia = static_cast<size_t>(cy) * stride_a +
                              static_cast<size_t>(cx * channels + c);
            const size_t ib = static_cast<size_t>(cy) * stride_b +
                              static_cast<size_t>(cx * channels + c);

            const double px = a.data[ia];
            const double py = b.data[ib];
            const double g = wy * w[static_cast<size_t>(dx + kHalo)];

            sx += g * px;
            sy += g * py;
            sxx += g * px * px;
            syy += g * py * py;
            sxy += g * px * py;
          }
        }

        const double vx = sxx - sx * sx;
        const double vy = syy - sy * sy;
        const double vxy = sxy - sx * sy;

        const double lum = (2.0 * sx * sy + kC1) / (sx * sx + sy * sy + kC1);
        const double den = vx + vy + kC2;
        const double cs = den > 0.0 ? (2.0 * vxy + kC2) / den : 0.0;

        cs_sum += cs;
        ssim_sum += lum * cs;
      }
    }
  }

  out->sq_sum = sq_sum;
  out->ssim_sum = ssim_sum;
  out->cs_sum = cs_sum;
  out->samples = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) *
                 static_cast<uint64_t>(channels);
  return true;
}

} // namespace

bool mse(const ImageView &a, const ImageView &b, double *out,
         std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "metrics: null output";
    return false;
  }

  ScaleStats s;
  if (!scaleStats(a, b, /*with_window=*/false, &s, err)) return false;

  const double n = static_cast<double>(s.samples);
  if (!(n > 0.0)) {
    if (err != nullptr) *err = "metrics: empty image";
    return false;
  }

  *out = s.sq_sum / n;
  return true;
}

bool ssim(const ImageView &a, const ImageView &b, SsimResult *out,
          std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "metrics: null output";
    return false;
  }
  *out = SsimResult{};

  ScaleStats s;
  if (!scaleStats(a, b, /*with_window=*/true, &s, err)) return false;

  const double n0 = static_cast<double>(s.samples);
  if (!(n0 > 0.0)) {
    if (err != nullptr) *err = "metrics: empty image";
    return false;
  }
  out->ssim = s.ssim_sum / n0;

  const int levels = msSsimLevels(a.width, a.height);
  out->levels = levels;

  double cs[kMsSsimMaxLevels] = {0.0, 0.0, 0.0, 0.0, 0.0};
  cs[0] = s.cs_sum / n0;

  double last_ssim = out->ssim;

  // 层级间只保留当前层与下一层：整座金字塔在大图上要白占几百 MB。
  std::vector<uint8_t> cur_a;
  std::vector<uint8_t> cur_b;
  std::vector<uint8_t> next_a;
  std::vector<uint8_t> next_b;

  int cur_w = a.width;
  int cur_h = a.height;

  for (int level = 1; level < levels; ++level) {
    ImageView src_a;
    ImageView src_b;

    if (level == 1) {
      src_a = a;
      src_b = b;
    } else {
      src_a.data = cur_a.data();
      src_a.width = cur_w;
      src_a.height = cur_h;
      src_a.channels = a.channels;
      src_b.data = cur_b.data();
      src_b.width = cur_w;
      src_b.height = cur_h;
      src_b.channels = a.channels;
    }

    int nw = 0;
    int nh = 0;
    if (!downsample2x(src_a, &next_a, &nw, &nh) ||
        !downsample2x(src_b, &next_b, &nw, &nh)) {
      if (err != nullptr) *err = "metrics: pyramid downsampling failed";
      return false;
    }

    ImageView la;
    la.data = next_a.data();
    la.width = nw;
    la.height = nh;
    la.channels = a.channels;

    ImageView lb;
    lb.data = next_b.data();
    lb.width = nw;
    lb.height = nh;
    lb.channels = a.channels;

    ScaleStats sl;
    if (!scaleStats(la, lb, /*with_window=*/true, &sl, err)) return false;

    const double nl = static_cast<double>(sl.samples);
    cs[level] = sl.cs_sum / nl;
    last_ssim = sl.ssim_sum / nl;

    cur_a = std::move(next_a);
    cur_b = std::move(next_b);
    cur_w = nw;
    cur_h = nh;
  }

  out->ms_ssim = msSsimFromScales(cs, last_ssim, levels);
  return true;
}

} // namespace reference
} // namespace metrics
} // namespace ocl
} // namespace image_codec
