/*
 * File: metrics_test.cpp
 * Brief: Metrics test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// metrics_test.cpp - 画质指标的 OpenCL 自检。
//
// 检查的不是「能跑通」，而是三件最容易悄悄错的事：
//   1. GPU 与 CPU 参考实现逐项对拍（band_rows = 16 的多 band 路径）；
//   2. 与另外两个后端（CUDA / Vulkan）给出**同一个数字**（golden 值）；
//   3. 常数图上的解析解（唯一不依赖「另一份实现」的检查）。
//
// 退出码：0 = 通过，2 = SKIP（无 OpenCL 设备或不支持 fp64），其它 = 失败。
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "metrics.h"

using image_codec::ocl::metrics::ImageView;
using image_codec::ocl::metrics::Options;
using image_codec::ocl::metrics::SsimResult;

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

// 确定性图案（纯渐变会让 LZW/滤波类缺陷被掩盖；这里要的是有局部随机性）。
// 与 Image.Codec.CUDA / Image.Codec.Vulkan 的同名测试逐字相同：三个后端必须
// 在同一张图上给出同一组数字。
std::vector<uint8_t> makeImage(int w, int h, int c) {
  std::vector<uint8_t> data(static_cast<size_t>(w) * h * c);
  std::mt19937 rng(12345);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (int ch = 0; ch < c; ++ch) {
        const size_t idx = (static_cast<size_t>(y) * w + x) * c + ch;
        const int v =
            (x * 3 + y * 5 + ch * 37 + static_cast<int>(rng() % 7)) & 0xFF;
        data[idx] = static_cast<uint8_t>(v);
      }
    }
  }
  return data;
}

ImageView view(const std::vector<uint8_t> &data, int w, int h, int c) {
  ImageView v;
  v.data = data.data();
  v.width = w;
  v.height = h;
  v.channels = c;
  return v;
}

// 每级都重新分配图像时最贵的是 SSIM 的 11x11 窗；这里只跑一次。
void testGpuVsCpu(const ImageView &a, const ImageView &b, const char *what) {
  Options opt;
  opt.device_index = 0;
  // band 越小，halo 行复制与归约次数越多：边界拼接错一位会立刻暴露成数字不一致，
  // 而默认 256 行的 band 在 300x200 的图上只有一两个 band，根本测不出来。
  opt.band_rows = 16;

  std::string err;

  double mse_gpu = -1.0;
  check(image_codec::ocl::metrics::mse(a, b, opt, &mse_gpu, &err), "mse gpu");
  double mse_cpu = -1.0;
  check(image_codec::ocl::metrics::reference::mse(a, b, &mse_cpu, &err),
        "mse cpu");
  check(std::fabs(mse_gpu - mse_cpu) < 1e-9, "mse gpu == cpu");

  double psnr_gpu = 0.0;
  check(image_codec::ocl::metrics::psnr(a, b, opt, &psnr_gpu, &err),
        "psnr gpu");
  const double psnr_expected =
      mse_cpu <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse_cpu);
  check(std::fabs(psnr_gpu - psnr_expected) < 1e-6, "psnr from mse");

  SsimResult ssim_gpu;
  check(image_codec::ocl::metrics::ssim(a, b, opt, &ssim_gpu, &err),
        "ssim gpu");
  SsimResult ssim_cpu;
  check(image_codec::ocl::metrics::reference::ssim(a, b, &ssim_cpu, &err),
        "ssim cpu");

  check(std::fabs(ssim_gpu.ssim - ssim_cpu.ssim) < 1e-9,
        "ssim gpu == cpu (banded halo)");
  check(std::fabs(ssim_gpu.ms_ssim - ssim_cpu.ms_ssim) < 1e-9,
        "ms-ssim gpu == cpu (banded halo)");
  check(ssim_gpu.levels == ssim_cpu.levels, "ms-ssim levels gpu == cpu");

  std::printf("%s: mse=%.6f psnr=%.4f dB ssim=%.9f ms-ssim=%.9f (%d 级)\n",
              what, mse_gpu, psnr_gpu, ssim_gpu.ssim, ssim_gpu.ms_ssim,
              ssim_gpu.levels);
}

// 三个后端共用的一组期望值（由 Image.Codec.CUDA 的指标实现给出）。
// 数值必须逐位一致：常数、窗、边界策略、金字塔方式各只有一份定义，
// 后端差异只允许出现在浮点累加顺序上（这里刻意用双精度归约把差异压到 1e-12 内）。
constexpr double kGoldenMse = 1.989219444444;
constexpr double kGoldenPsnr = 45.143976650968;
constexpr double kGoldenSsim = 0.992878374042;
constexpr double kGoldenMsSsim = 0.999443606477;
constexpr int kGoldenLevels = 5;

void testGolden() {
  const int w = 400;
  const int h = 300;
  const int c = 3;

  const std::vector<uint8_t> base = makeImage(w, h, c);
  std::vector<uint8_t> noisy = makeImage(w, h, c);
  std::mt19937 rng(99);
  for (auto &v : noisy) {
    const int d = static_cast<int>(rng() % 5) - 2;
    const int nv = static_cast<int>(v) + d;
    v = static_cast<uint8_t>(nv < 0 ? 0 : (nv > 255 ? 255 : nv));
  }

  const ImageView a = view(base, w, h, c);
  const ImageView b = view(noisy, w, h, c);

  Options opt;
  std::string err;

  double mse = 0.0;
  check(image_codec::ocl::metrics::mse(a, b, opt, &mse, &err), "golden mse");
  check(std::fabs(mse - kGoldenMse) < 1e-9, "golden mse matches CUDA");

  double psnr = 0.0;
  check(image_codec::ocl::metrics::psnr(a, b, opt, &psnr, &err), "golden psnr");
  check(std::fabs(psnr - kGoldenPsnr) < 1e-9, "golden psnr matches CUDA");

  SsimResult s;
  check(image_codec::ocl::metrics::ssim(a, b, opt, &s, &err), "golden ssim");
  check(std::fabs(s.ssim - kGoldenSsim) < 1e-9, "golden ssim matches CUDA");
  check(std::fabs(s.ms_ssim - kGoldenMsSsim) < 1e-9,
        "golden ms-ssim matches CUDA");
  check(s.levels == kGoldenLevels, "golden levels matches CUDA");
}

// 常数图对：sigma = 0，CS 恒为 1，SSIM 退化成亮度项，可以手算。
void testAnalytic() {
  const int w = 400;
  const int h = 300;
  std::vector<uint8_t> ba(static_cast<size_t>(w) * h * 3, 100);
  std::vector<uint8_t> bb(static_cast<size_t>(w) * h * 3, 110);

  const ImageView a = view(ba, w, h, 3);
  const ImageView b = view(bb, w, h, 3);

  const double c1 = (0.01 * 255.0) * (0.01 * 255.0);
  const double expected_ssim =
      (2.0 * 100.0 * 110.0 + c1) / (100.0 * 100.0 + 110.0 * 110.0 + c1);

  Options opt;
  std::string err;
  SsimResult r;
  if (!image_codec::ocl::metrics::ssim(a, b, opt, &r, &err)) {
    std::fprintf(stderr, "FAIL: analytic ssim (%s)\n", err.c_str());
    ++g_failures;
    return;
  }
  check(std::fabs(r.ssim - expected_ssim) < 1e-9,
        "ssim matches luminance-only");

  // CS 恒为 1，所以 MS-SSIM 只剩最末一级的 SSIM 按其归一化后的权重开方。
  const double weight_last =
      0.1333 / (0.0448 + 0.2856 + 0.3001 + 0.2363 + 0.1333);
  check(std::fabs(r.ms_ssim - std::pow(expected_ssim, weight_last)) < 1e-9,
        "ms-ssim matches single-scale term");

  double m = 0.0;
  check(image_codec::ocl::metrics::mse(a, b, opt, &m, &err), "analytic mse");
  check(std::fabs(m - 100.0) < 1e-9, "mse == 10^2");
}

} // namespace

int main() {
  // 先探测设备：没有 OpenCL / 不支持 fp64 时明确 SKIP，而不是报一堆假失败。
  Options probe;
  std::string err;
  const std::vector<uint8_t> tiny = makeImage(32, 32, 1);
  const ImageView tv = view(tiny, 32, 32, 1);
  double probe_value = 0.0;
  if (!image_codec::ocl::metrics::mse(tv, tv, probe, &probe_value, &err)) {
    std::printf("SKIP: %s\n", err.c_str());
    return 2;
  }

  // 1. 完全相同 / 有扰动 / 灰度 / 小图：GPU 必须与 CPU 参考逐位对齐。
  const int w = 400;
  const int h = 300;
  const std::vector<uint8_t> a = makeImage(w, h, 3);
  const ImageView va = view(a, w, h, 3);
  testGpuVsCpu(va, va, "identical");

  std::vector<uint8_t> perturbed = makeImage(w, h, 3);
  std::mt19937 rng(7);
  for (auto &v : perturbed) {
    const int d = static_cast<int>(rng() % 5) - 2;
    const int nv = static_cast<int>(v) + d;
    v = static_cast<uint8_t>(nv < 0 ? 0 : (nv > 255 ? 255 : nv));
  }
  testGpuVsCpu(va, view(perturbed, w, h, 3), "perturbed");

  const std::vector<uint8_t> g = makeImage(97, 71, 1);
  testGpuVsCpu(view(g, 97, 71, 1), view(g, 97, 71, 1), "gray identical");

  // 2. 三个后端的一致性基准。
  testGolden();

  // 3. 解析解。
  testAnalytic();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}
