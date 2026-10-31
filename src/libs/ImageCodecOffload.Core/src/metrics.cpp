/*
 * File: metrics.cpp
 * Brief: Metrics.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// metrics.cpp - 画质指标的 CPU 参考实现。
//
// 与子项目的 metrics_cpu.cpp 逐项对应：SSIM 的常数、边界策略、金字塔下采样
// 方式只要有一处不同，两边的数字就会对不上，而这种偏差在真实图像上往往只差
// 0.001——肉眼与「看起来合理」都发现不了。所以这里刻意不做任何优化。
#include "metrics.h"

#include <cmath>
#include <vector>

namespace image_codec {
namespace offload {
namespace metrics {

namespace {

struct ScaleStats {
  double sq_sum = 0.0;
  double ssim_sum = 0.0;
  double cs_sum = 0.0;
  uint64_t samples = 0;
};

// 单尺度统计。with_window == false 时只累加平方误差。
void scaleStats(const Image& a, const Image& b, bool with_window,
                ScaleStats* out) {
  const int width = a.width;
  const int height = a.height;
  const int channels = a.channels;

  double w[kWindow];
  gaussianWeights11(w);

  double sq_sum = 0.0;
  double ssim_sum = 0.0;
  double cs_sum = 0.0;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < channels; ++c) {
        const size_t o =
            (static_cast<size_t>(y) * width + x) * channels + c;
        const double d =
            static_cast<double>(a.pixels[o]) - static_cast<double>(b.pixels[o]);
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

            const size_t oo = (static_cast<size_t>(cy) * width + cx) * channels +
                              c;
            const double px = a.pixels[oo];
            const double py = b.pixels[oo];
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
}

// 2x2 均值下采样（与子项目的 downsample2x 完全一致）。
bool downsample2x(const Image& src, Image* dst) {
  const int w = src.width / 2;
  const int h = src.height / 2;
  if (w <= 0 || h <= 0) return false;

  const int c = src.channels;
  dst->width = w;
  dst->height = h;
  dst->channels = c;
  dst->pixels.assign(static_cast<size_t>(w) * h * c, 0);

  const size_t src_stride = src.rowBytes();
  const size_t dst_stride = dst->rowBytes();

  // 单层循环，列号用计数递推，避免每个像素做一次除法。
  const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
  size_t done = 0;
  int y = 0;
  int x = 0;
  while (done < total) {
    const uint8_t* r0 = src.pixels.data() + static_cast<size_t>(2 * y) * src_stride;
    const uint8_t* r1 = r0 + src_stride;
    const size_t o0 = static_cast<size_t>(2 * x) * static_cast<size_t>(c);
    const size_t o1 = o0 + static_cast<size_t>(c);
    const size_t o = done * static_cast<size_t>(c);
    for (int k = 0; k < c; ++k) {
      const unsigned sum = static_cast<unsigned>(r0[o0 + k]) +
                           static_cast<unsigned>(r0[o1 + k]) +
                           static_cast<unsigned>(r1[o0 + k]) +
                           static_cast<unsigned>(r1[o1 + k]);
      dst->pixels[o + static_cast<size_t>(k)] =
          static_cast<uint8_t>((sum + 2u) / 4u);
    }
    ++done;
    ++x;
    if (x == w) {
      x = 0;
      ++y;
    }
  }
  return true;
}

double msSsimFromScales(const double* cs, double ssim_last, int levels) {
  if (cs == nullptr || levels < 1 || levels > kMsSsimMaxLevels) return 0.0;

  double weight_sum = 0.0;
  for (int j = 0; j < levels; ++j) weight_sum += kMsSsimWeights[j];
  if (!(weight_sum > 0.0)) return 0.0;

  // 对数域相乘：CS 与 SSIM 都可能非常接近 1，直接连乘再乘 5 个权重会让
  // 中间结果沉到 double 的精度边缘。
  double log_sum = 0.0;
  for (int j = 0; j < levels - 1; ++j) {
    if (!(cs[j] > 0.0)) return 0.0;  // 负相关/退化区域：MS-SSIM 无定义
    log_sum += (kMsSsimWeights[j] / weight_sum) * std::log(cs[j]);
  }
  if (!(ssim_last > 0.0)) return 0.0;
  log_sum += (kMsSsimWeights[levels - 1] / weight_sum) * std::log(ssim_last);
  return std::exp(log_sum);
}

Result<void> validatePair(const Image& a, const Image& b) {
  if (!a.valid() || !b.valid()) {
    return fail<void>(Status::InvalidArgument, "metrics: invalid image buffer");
  }
  if (a.width != b.width || a.height != b.height || a.channels != b.channels) {
    return fail<void>(Status::InvalidArgument,
                      "metrics: size/channel mismatch");
  }
  return {};
}

}  // namespace

void gaussianWeights11(double* out) {
  if (out == nullptr) return;

  double raw[kWindow];
  double sum = 0.0;
  for (int i = 0; i < kWindow; ++i) {
    const double d = static_cast<double>(i - kHalo);
    raw[i] = std::exp(-(d * d) / (2.0 * kSigma * kSigma));
    sum += raw[i];
  }
  for (int i = 0; i < kWindow; ++i) out[i] = raw[i] / sum;
}

double psnrFromMse(double mse_value) {
  if (!(mse_value > 0.0)) return 99.0;
  return 10.0 * std::log10(255.0 * 255.0 / mse_value);
}

int msSsimLevels(int width, int height) {
  int levels = 1;
  int w = width;
  int h = height;
  while (levels < kMsSsimMaxLevels) {
    const int nw = w / 2;
    const int nh = h / 2;
    if (nw < kWindow || nh < kWindow) break;
    w = nw;
    h = nh;
    ++levels;
  }
  return levels;
}

Result<double> mse(const Image& a, const Image& b) {
  if (auto ok = validatePair(a, b); !ok) return std::unexpected(ok.error());

  ScaleStats s;
  scaleStats(a, b, /*with_window=*/false, &s);

  const double n = static_cast<double>(s.samples);
  if (!(n > 0.0)) return fail<double>(Status::InvalidArgument, "metrics: empty image");

  return s.sq_sum / n;
}

Result<double> psnr(const Image& a, const Image& b) {
  const Result<double> m = mse(a, b);
  if (!m) return std::unexpected(m.error());
  return psnrFromMse(*m);
}

Result<SsimResult> ssim(const Image& a, const Image& b) {
  if (auto ok = validatePair(a, b); !ok) return std::unexpected(ok.error());

  ScaleStats s;
  scaleStats(a, b, /*with_window=*/true, &s);

  const double n0 = static_cast<double>(s.samples);
  if (!(n0 > 0.0)) {
    return fail<SsimResult>(Status::InvalidArgument, "metrics: empty image");
  }

  SsimResult out;
  out.ssim = s.ssim_sum / n0;
  out.levels = msSsimLevels(a.width, a.height);

  double cs[kMsSsimMaxLevels] = {0.0, 0.0, 0.0, 0.0, 0.0};
  cs[0] = s.cs_sum / n0;

  double last_ssim = out.ssim;

  // 层级间只保留当前层与下一层：整座金字塔在大图上要白占几百 MB。
  Image cur;
  Image next;
  const Image* src = &a;
  const Image* src_b = &b;
  Image cur_b;

  for (int level = 1; level < out.levels; ++level) {
    Image next_b;
    if (!downsample2x(*src, &next) || !downsample2x(*src_b, &next_b)) {
      return fail<SsimResult>(Status::Internal,
                              "metrics: pyramid downsampling failed");
    }

    ScaleStats sl;
    scaleStats(next, next_b, /*with_window=*/true, &sl);

    const double nl = static_cast<double>(sl.samples);
    cs[level] = sl.cs_sum / nl;
    last_ssim = sl.ssim_sum / nl;

    cur = std::move(next);
    cur_b = std::move(next_b);
    src = &cur;
    src_b = &cur_b;
  }

  out.ms_ssim = msSsimFromScales(cs, last_ssim, out.levels);
  return out;
}

}  // namespace metrics
}  // namespace offload
}  // namespace image_codec
