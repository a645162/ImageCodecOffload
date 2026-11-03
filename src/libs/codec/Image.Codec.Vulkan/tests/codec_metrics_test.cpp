// File      : codec_metrics_test.cpp
// Brief     : Vulkan 指标与 CPU 参考 / golden 常量对拍
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 退出码：0 = PASS，2 = SKIP（没有可用 Vulkan 设备 / 不支持 fp64），1 = FAIL。
//
// 对拍分两层：
//   1. GPU 与同仓库的 CPU 参考实现逐项比较（这是「后端有没有算错」）；
//   2. CPU 参考与父项目锁死的 golden 常量比较（这是「三个后端是不是同一个定义」）。
// 只有两层都过，才能说「换后端不换数字」。
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "image_buffer.h"
#include "metrics.h"
#include "vk_codec_device.h"

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

void checkNear(double got, double want, double tolerance,
               const std::string &what) {
  if (!(std::fabs(got - want) <= tolerance)) {
    std::printf("FAIL: %s (got %.15f, want %.15f, tol %g)\n", what.c_str(), got,
                want, tolerance);
    ++g_failures;
    return;
  }
  std::printf("  ok  %-28s %.12f\n", what.c_str(), got);
}

// 与父项目 tests/ico_test.cpp 的 makeImage()/perturb() 逐行相同（同 seed、
// 同 mt19937 用法）：golden 常量就是为这一张图锁定的，图案差一个随机数
// 就会对不上——而这种偏差看上去非常像「GPU 算错了」。
image_codec::vulkan::ImageBuffer makePattern() {
  using image_codec::vulkan::ImageBuffer;
  ImageBuffer image;
  image.allocate(400, 300, 3);

  std::mt19937 rng(12345);
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      for (int ch = 0; ch < 3; ++ch) {
        const size_t idx = (static_cast<size_t>(y) * image.width + x) * 3 + ch;
        const int v =
            (x * 3 + y * 5 + ch * 37 + static_cast<int>(rng() % 7)) & 0xFF;
        image.data[idx] = static_cast<uint8_t>(v);
      }
    }
  }
  return image;
}

image_codec::vulkan::ImageBuffer
makeDisturbed(const image_codec::vulkan::ImageBuffer &src) {
  image_codec::vulkan::ImageBuffer out = src;

  std::mt19937 rng(99);
  for (auto &v : out.data) {
    const int d = static_cast<int>(rng() % 5) - 2;
    const int nv = static_cast<int>(v) + d;
    v = static_cast<uint8_t>(nv < 0 ? 0 : (nv > 255 ? 255 : nv));
  }
  return out;
}

} // namespace

int main() {
  using namespace image_codec::vulkan;

  // 设备探测：没有 Vulkan 或没有 fp64 时明确 SKIP（不是 PASS，也不是 FAIL）。
  Device probe;
  std::string device_error;
  if (!probe.init(0, &device_error)) {
    std::printf("SKIP: %s\n", device_error.c_str());
    return 2;
  }
  std::printf("device: %s (fp64=%s, memory_budget_ext=%s)\n",
              probe.caps().name.c_str(), probe.caps().fp64 ? "yes" : "no",
              probe.caps().memory_budget_ext ? "yes" : "no");
  {
    const MemoryInfo info = probe.memoryInfo();
    std::printf("memory: total=%zu MiB free=%zu MiB max_alloc=%zu MiB\n",
                info.total_bytes >> 20, info.free_bytes >> 20,
                info.max_alloc_bytes >> 20);
  }

  const ImageBuffer a = makePattern();
  const ImageBuffer b = makeDisturbed(a);

  // ---- CPU 参考 vs golden（与 CUDA / OpenCL 共用）----
  double ref_mse = 0.0;
  std::string error;
  check(metrics::reference::mse(a, b, &ref_mse, &error),
        "reference mse: " + error);

  constexpr double kGoldenMse = 1.989219444444;
  constexpr double kGoldenSsim = 0.992878374042;
  constexpr double kGoldenMsSsim = 0.999443606477;
  constexpr int kGoldenLevels = 5;

  checkNear(ref_mse, kGoldenMse, 1e-9, "reference mse vs golden");

  metrics::SsimResult ref_ssim;
  check(metrics::reference::ssim(a, b, &ref_ssim, &error),
        "reference ssim: " + error);
  checkNear(ref_ssim.ssim, kGoldenSsim, 1e-9, "reference ssim vs golden");
  checkNear(ref_ssim.ms_ssim, kGoldenMsSsim, 1e-9,
            "reference ms-ssim vs golden");
  check(ref_ssim.levels == kGoldenLevels, "reference levels == 5");

  // ---- GPU vs CPU 参考 ----
  metrics::Options options;
  options.device_index = 0;
  options.use_gpu = true;

  double gpu_mse = 0.0;
  check(metrics::mse(a, b, options, &gpu_mse, &error), "gpu mse: " + error);
  checkNear(gpu_mse, ref_mse, 1e-9, "gpu mse vs cpu");

  double gpu_psnr = 0.0;
  check(metrics::psnr(a, b, options, &gpu_psnr, &error), "gpu psnr: " + error);
  checkNear(gpu_psnr, 10.0 * std::log10(255.0 * 255.0 / kGoldenMse), 1e-9,
            "gpu psnr vs golden");

  metrics::SsimResult gpu_ssim;
  check(metrics::ssim(a, b, options, &gpu_ssim, &error), "gpu ssim: " + error);
  checkNear(gpu_ssim.ssim, ref_ssim.ssim, 1e-9, "gpu ssim vs cpu");
  checkNear(gpu_ssim.ms_ssim, ref_ssim.ms_ssim, 1e-9, "gpu ms-ssim vs cpu");
  check(gpu_ssim.levels == ref_ssim.levels, "gpu levels match cpu");

  // ---- band 拼接：把 band_rows 调到 8，验证 halo 行真的接得上 ----
  metrics::Options small_bands = options;
  small_bands.band_rows = 8;
  double banded_mse = 0.0;
  check(metrics::mse(a, b, small_bands, &banded_mse, &error),
        "gpu mse (band_rows=8): " + error);
  checkNear(banded_mse, ref_mse, 1e-9, "gpu mse (band_rows=8) vs cpu");

  metrics::SsimResult banded_ssim;
  check(metrics::ssim(a, b, small_bands, &banded_ssim, &error),
        "gpu ssim (band_rows=8): " + error);
  checkNear(banded_ssim.ssim, ref_ssim.ssim, 1e-9,
            "gpu ssim (band_rows=8) vs cpu");

  // ---- 相同图：MSE == 0 -> PSNR 固定 99.0，SSIM == 1 ----
  double same_mse = 0.0;
  check(metrics::mse(a, a, options, &same_mse, &error),
        "gpu mse identical: " + error);
  checkNear(same_mse, 0.0, 0.0, "gpu mse identical == 0");

  double same_psnr = 0.0;
  check(metrics::psnr(a, a, options, &same_psnr, &error),
        "gpu psnr identical: " + error);
  checkNear(same_psnr, 99.0, 0.0, "gpu psnr identical == 99");

  metrics::SsimResult same_ssim;
  check(metrics::ssim(a, a, options, &same_ssim, &error),
        "gpu ssim identical: " + error);
  checkNear(same_ssim.ssim, 1.0, 1e-12, "gpu ssim identical == 1");
  checkNear(same_ssim.ms_ssim, 1.0, 1e-12, "gpu ms-ssim identical == 1");

  // ---- 显存上限：上限小于 band 缓冲时必须明确失败并给出原因 ----
  metrics::Options tiny_budget = options;
  tiny_budget.memory_limit = 1024; // 远小于一张 400x300 的 band
  double budget_mse = 0.0;
  const bool budget_ok = metrics::mse(a, b, tiny_budget, &budget_mse, &error);
  check(!budget_ok, "memory limit rejects oversized band");
  if (!budget_ok) std::printf("  ok  memory limit reason: %s\n", error.c_str());

  // ---- 单通道 / 双通道（通道语义必须一致）----
  ImageBuffer gray;
  gray.allocate(64, 64, 1);
  for (size_t i = 0; i < gray.data.size(); ++i) {
    gray.data[i] = static_cast<uint8_t>(i % 251);
  }
  ImageBuffer gray2 = gray;
  for (size_t i = 0; i < gray2.data.size(); ++i) {
    gray2.data[i] = static_cast<uint8_t>(gray2.data[i] ^ 1u);
  }
  for (const int channels : {1, 2, 3, 4}) {
    ImageBuffer x;
    x.allocate(64, 64, channels);
    for (size_t i = 0; i < x.data.size(); ++i) {
      x.data[i] = static_cast<uint8_t>((i * 37 + channels) % 256);
    }
    ImageBuffer y = x;
    for (size_t i = 0; i < y.data.size(); i += 3) {
      y.data[i] = static_cast<uint8_t>(y.data[i] ^ 0x5Au);
    }
    double cpu_mse = 0.0;
    double gpu_value = 0.0;
    check(metrics::reference::mse(x, y, &cpu_mse, &error),
          "channels mse reference: " + error);
    check(metrics::mse(x, y, options, &gpu_value, &error),
          "channels mse gpu: " + error);
    checkNear(gpu_value, cpu_mse, 1e-9,
              "gpu mse matches cpu, channels=" + std::to_string(channels));
  }

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}
