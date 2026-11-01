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
// 0.001——肉眼与「看起来合理」都发现不了。所以这里刻意不做任何 SIMD 与循环
// 展开，也不改变累加顺序。
//
// 唯一被并行的是「逐样本 11x11 高斯窗」这段纯计算：每个 band 的样本值先进
// 缓冲，再由单线程按原顺序累加，因此结果与串行逐位相同（见 metricsThreadCount
// 的说明）。大图 SSIM 从分钟级降到十秒级，而 golden 常量一个也不用动。
#include "metrics.h"

#include <algorithm>
#include <barrier>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

namespace image_codec {
namespace offload {
namespace metrics {

namespace {

// 一个 band 的样本值缓冲上限。32 MiB 对应 32000x3 的图约 14 行：
// 再大就会在进程本来就吃满几十 GB 的大图任务上又加几百 MB。
constexpr size_t kBandBudgetBytes = 32u * 1024u * 1024u;
// 线程数上限：再往上 barrier 与内存带宽的收益已经翻转，反而更慢。
constexpr int kMaxMetricsThreads = 64;

// 线程数来源：默认 hardware_concurrency；ICO_METRICS_THREADS 可覆盖
// （0 或负数 = 强制串行），用于「并行结果与串行逐位相同」的现场验证。
int metricsThreadCount() {
  if (const char *env = std::getenv("ICO_METRICS_THREADS"); env != nullptr) {
    const int requested = std::atoi(env);
    if (requested < 1) return 1;
    return std::min(requested, kMaxMetricsThreads);
  }
  const unsigned hardware = std::thread::hardware_concurrency();
  if (hardware == 0) return 1;
  return std::min(static_cast<int>(hardware), kMaxMetricsThreads);
}

// band 高度按「样本值缓冲不超过 kBandBudgetBytes」反推；窗口统计需要
// with_window ? 3 : 1 个 double/样本。
int bandRowsFor(const int width, const int channels, const bool with_window,
                const int height) {
  const size_t doubles_per_sample = with_window ? 3u : 1u;
  const size_t samples_per_band = std::max<size_t>(
      1u, kBandBudgetBytes / (sizeof(double) * doubles_per_sample));
  const size_t row_samples =
      std::max<size_t>(1u, static_cast<size_t>(width) * channels);
  const size_t rows = std::max<size_t>(1u, samples_per_band / row_samples);
  return static_cast<int>(std::min<size_t>(rows, static_cast<size_t>(height)));
}

struct ScaleStats {
  double sq_sum = 0.0;
  double ssim_sum = 0.0;
  double cs_sum = 0.0;
  uint64_t samples = 0;
};

// 单个 (像素, 通道) 的平方误差与高斯窗统计。运算顺序与串行版本逐项相同：
// 先 dy 后 dx、先 sx/sy 后 sxx/syy/sxy，最后按同一公式合成 CS 与 SSIM。
inline void sampleStats(const Image &a, const Image &b, const double *w,
                        const bool with_window, const int width,
                        const int height, const int channels, const int y,
                        const int x, const int c, double *sq, double *cs_out,
                        double *ssim_out) {
  const size_t o = (static_cast<size_t>(y) * width + x) * channels + c;
  const double d =
      static_cast<double>(a.pixels[o]) - static_cast<double>(b.pixels[o]);
  *sq = d * d;

  if (!with_window) return;

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

      const size_t oo = (static_cast<size_t>(cy) * width + cx) * channels + c;
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

  *cs_out = cs;
  *ssim_out = lum * cs;
}

// 单尺度统计。with_window == false 时只累加平方误差。
//
// 并行形态：band 内按行切给各线程算样本值 → barrier → 0 号线程按行、列、通道
// 顺序累加 → barrier。累加顺序与串行实现完全一致，因此结果逐位相同；
// 而 121 次乘加的窗口计算（占全部耗时的 99%）被摊到多核上。
void scaleStats(const Image &a, const Image &b, const bool with_window,
                ScaleStats *out) {
  const int width = a.width;
  const int height = a.height;
  const int channels = a.channels;

  double w[kWindow];
  gaussianWeights11(w);

  const int band_rows = bandRowsFor(width, channels, with_window, height);
  const int band_count = (height + band_rows - 1) / band_rows;
  const size_t max_samples = static_cast<size_t>(band_rows) *
                             static_cast<size_t>(width) *
                             static_cast<size_t>(channels);

  std::vector<double> sq_values(max_samples, 0.0);
  std::vector<double> cs_values(with_window ? max_samples : 0, 0.0);
  std::vector<double> ssim_values(with_window ? max_samples : 0, 0.0);

  double sq_sum = 0.0;
  double ssim_sum = 0.0;
  double cs_sum = 0.0;

  const int workers = metricsThreadCount();
  std::barrier sync(workers);

  const auto run = [&](const int worker) {
    for (int band = 0; band < band_count; ++band) {
      const int y0 = band * band_rows;
      const int rows = std::min(band_rows, height - y0);
      const int row_begin =
          static_cast<int>(static_cast<long long>(rows) * worker / workers);
      const int row_end = static_cast<int>(static_cast<long long>(rows) *
                                           (worker + 1) / workers);

      for (int r = row_begin; r < row_end; ++r) {
        const int y = y0 + r;
        size_t index = static_cast<size_t>(r) * width * channels;
        for (int x = 0; x < width; ++x) {
          for (int c = 0; c < channels; ++c, ++index) {
            double sq = 0.0;
            double cs = 0.0;
            double ssim = 0.0;
            sampleStats(a, b, w, with_window, width, height, channels, y, x, c,
                        &sq, &cs, &ssim);
            sq_values[index] = sq;
            if (with_window) {
              cs_values[index] = cs;
              ssim_values[index] = ssim;
            }
          }
        }
      }

      if (workers > 1) sync.arrive_and_wait();
      if (worker == 0) {
        const size_t samples = static_cast<size_t>(rows) * width * channels;
        for (size_t i = 0; i < samples; ++i) {
          sq_sum += sq_values[i];
          if (with_window) {
            cs_sum += cs_values[i];
            ssim_sum += ssim_values[i];
          }
        }
      }
      if (workers > 1) sync.arrive_and_wait();
    }
  };

  if (workers == 1) {
    run(0);
  } else {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(workers - 1));
    for (int worker = 1; worker < workers; ++worker) {
      threads.emplace_back(run, worker);
    }
    run(0);
    for (std::thread &thread : threads) thread.join();
  }

  out->sq_sum = sq_sum;
  out->ssim_sum = ssim_sum;
  out->cs_sum = cs_sum;
  out->samples = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) *
                 static_cast<uint64_t>(channels);
}

// 2x2 均值下采样（与子项目的 downsample2x 完全一致）。
// 输出像素之间互不依赖，按行切给多线程不会改变任何一个字节。
bool downsample2x(const Image &src, Image *dst) {
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

  const auto runRows = [&](const int y_begin, const int y_end) {
    for (int y = y_begin; y < y_end; ++y) {
      const uint8_t *r0 =
          src.pixels.data() + static_cast<size_t>(2 * y) * src_stride;
      const uint8_t *r1 = r0 + src_stride;
      uint8_t *row = dst->pixels.data() + static_cast<size_t>(y) * dst_stride;
      for (int x = 0; x < w; ++x) {
        const size_t o0 = static_cast<size_t>(2 * x) * static_cast<size_t>(c);
        const size_t o1 = o0 + static_cast<size_t>(c);
        for (int k = 0; k < c; ++k) {
          const unsigned sum = static_cast<unsigned>(r0[o0 + k]) +
                               static_cast<unsigned>(r0[o1 + k]) +
                               static_cast<unsigned>(r1[o0 + k]) +
                               static_cast<unsigned>(r1[o1 + k]);
          row[static_cast<size_t>(x) * static_cast<size_t>(c) +
              static_cast<size_t>(k)] = static_cast<uint8_t>((sum + 2u) / 4u);
        }
      }
    }
  };

  // 小图上开线程的代价（几百微秒）比计算本身还高，按目的地行数做个门槛。
  const int workers = std::min(metricsThreadCount(), h);
  if (workers <= 1 || h < 64) {
    runRows(0, h);
    return true;
  }
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(workers - 1));
  for (int worker = 1; worker < workers; ++worker) {
    const int begin =
        static_cast<int>(static_cast<long long>(h) * worker / workers);
    const int end =
        static_cast<int>(static_cast<long long>(h) * (worker + 1) / workers);
    threads.emplace_back(runRows, begin, end);
  }
  runRows(0, static_cast<int>(static_cast<long long>(h) / workers));
  for (std::thread &thread : threads) thread.join();
  return true;
}

double msSsimFromScales(const double *cs, double ssim_last, int levels) {
  if (cs == nullptr || levels < 1 || levels > kMsSsimMaxLevels) return 0.0;

  double weight_sum = 0.0;
  for (int j = 0; j < levels; ++j) weight_sum += kMsSsimWeights[j];
  if (!(weight_sum > 0.0)) return 0.0;

  // 对数域相乘：CS 与 SSIM 都可能非常接近 1，直接连乘再乘 5 个权重会让
  // 中间结果沉到 double 的精度边缘。
  double log_sum = 0.0;
  for (int j = 0; j < levels - 1; ++j) {
    if (!(cs[j] > 0.0)) return 0.0; // 负相关/退化区域：MS-SSIM 无定义
    log_sum += (kMsSsimWeights[j] / weight_sum) * std::log(cs[j]);
  }
  if (!(ssim_last > 0.0)) return 0.0;
  log_sum += (kMsSsimWeights[levels - 1] / weight_sum) * std::log(ssim_last);
  return std::exp(log_sum);
}

Result<void> validatePair(const Image &a, const Image &b) {
  if (!a.valid() || !b.valid()) {
    return fail<void>(Status::InvalidArgument, "metrics: invalid image buffer");
  }
  if (a.width != b.width || a.height != b.height || a.channels != b.channels) {
    return fail<void>(Status::InvalidArgument,
                      "metrics: size/channel mismatch");
  }
  return {};
}

} // namespace

void gaussianWeights11(double *out) {
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

Result<double> mse(const Image &a, const Image &b) {
  if (auto ok = validatePair(a, b); !ok) return std::unexpected(ok.error());

  ScaleStats s;
  scaleStats(a, b, /*with_window=*/false, &s);

  const double n = static_cast<double>(s.samples);
  if (!(n > 0.0))
    return fail<double>(Status::InvalidArgument, "metrics: empty image");

  return s.sq_sum / n;
}

Result<double> psnr(const Image &a, const Image &b) {
  const Result<double> m = mse(a, b);
  if (!m) return std::unexpected(m.error());
  return psnrFromMse(*m);
}

Result<SsimResult> ssim(const Image &a, const Image &b) {
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
  const Image *src = &a;
  const Image *src_b = &b;
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

} // namespace metrics
} // namespace offload
} // namespace image_codec
