/*
 * File: metrics.h
 * Brief: Metrics.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// metrics.h - 画质指标（MSE / PSNR / SSIM / MS-SSIM）的 OpenCL 实现入口。
//
// 与 CUDA 后端（Image.Codec.CUDA::Metrics）共用同一套定义，见
// Image.Codec.CUDA/docs/metrics.md。三个后端（OpenCL / CUDA / Vulkan）必须给出
// 相同的数字，否则「换了后端画质变了」这种结论无从判断。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace image_codec {
namespace ocl {
namespace metrics {

// 非拥有视图：数据由调用方持有（C API 的 image 结构自己 malloc）。
// stride == 0 表示紧凑排列（width * channels）。
struct ImageView {
  const uint8_t *data = nullptr;
  int width = 0;
  int height = 0;
  int channels = 0;
  size_t stride = 0;

  size_t rowBytes() const {
    return stride != 0
               ? stride
               : static_cast<size_t>(width) * static_cast<size_t>(channels);
  }

  bool valid() const {
    return data != nullptr && width > 0 && height > 0 && channels > 0;
  }
};

struct Options {
  int device_index = 0;
  // 关掉 GPU 时直接走 reference::*（用于在没有可用设备的机器上仍然能测）。
  bool use_gpu = true;
  // 每个 band 的行数。显存占用 = (band_rows + 2*halo) * 行字节，与图像高度无关。
  // 小图上调小它是验证 halo 拼接正确性的主要手段。
  int band_rows = 256;
};

struct SsimResult {
  double ssim = 0.0;
  double ms_ssim = 0.0;
  int levels = 0; // MS-SSIM 实际使用的尺度数（1..5）
};

// ---- 三个独立入口 ----
// 为什么不合成一个 compare()：11x11 窗的内存流量是纯平方误差的上百倍，
// 只想筛掉编码器坏输出的调用方不该白付这笔钱。SSIM 与 MS-SSIM 合并在一个入口，
// 是因为 MS-SSIM 的第 0 级就是 SSIM 的那一次统计，拆开只会把最贵的一级算两遍。
bool mse(const ImageView &a, const ImageView &b, const Options &options,
         double *out, std::string *err);

bool psnr(const ImageView &a, const ImageView &b, const Options &options,
          double *out, std::string *err);

bool ssim(const ImageView &a, const ImageView &b, const Options &options,
          SsimResult *out, std::string *err);

// ---- CPU 参考实现 ----
// 存在的意义是给 GPU kernel 一个可以逐项对拍的基准（见 docs/metrics.md）。
// 刻意「照公式直译」，不要在这里做性能优化。
namespace reference {

bool mse(const ImageView &a, const ImageView &b, double *out, std::string *err);

bool ssim(const ImageView &a, const ImageView &b, SsimResult *out,
          std::string *err);

} // namespace reference

} // namespace metrics
} // namespace ocl
} // namespace image_codec
