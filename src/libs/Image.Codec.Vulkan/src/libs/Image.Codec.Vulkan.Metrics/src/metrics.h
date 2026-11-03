// File      : metrics.h
// Brief     : 画质指标（MSE / PSNR / SSIM / MS-SSIM）的 Vulkan 实现入口
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 与 Image.Codec.CUDA::Metrics / Image.Codec.OpenCL 的 Metrics 共用同一套定义。
// 三个后端必须给出相同的数字，golden 常量锁在父项目的 tests/ico_test.cpp。
//
// 三个独立入口而不是一个 compare()：11x11 窗的内存流量是纯平方误差的上百倍，
// 只想筛掉编码器坏输出的调用方不该白付这笔钱。SSIM 与 MS-SSIM 合并在一个入口，
// 因为 MS-SSIM 的第 0 级就是 SSIM 的那一次统计。
#pragma once

#include <cstddef>
#include <string>

#include "image_buffer.h"

namespace image_codec {
namespace vulkan {
namespace metrics {

struct Options {
  int device_index = 0;
  // 关掉 GPU 时直接走 reference::*（没有可用设备的机器上仍然能测）。
  bool use_gpu = true;
  // 每个 band 的行数。显存占用 = (band_rows + 2*halo) * 行字节，与图像高度无关；
  // 小图上调小它是验证 halo 拼接正确性的主要手段。
  int band_rows = 256;
  // 显存上限（0 = 不限制）。超过后分配直接失败并说明原因。
  size_t memory_limit = 0;
};

struct SsimResult {
  double ssim = 0.0;
  double ms_ssim = 0.0;
  int levels = 0; // MS-SSIM 实际使用的尺度数（1..5）
};

bool mse(const ImageBuffer &a, const ImageBuffer &b, const Options &options,
         double *out, std::string *err);

bool psnr(const ImageBuffer &a, const ImageBuffer &b, const Options &options,
          double *out, std::string *err);

bool ssim(const ImageBuffer &a, const ImageBuffer &b, const Options &options,
          SsimResult *out, std::string *err);

// ---- CPU 参考实现 ----
// 存在的意义是给 GPU 着色器一个可以逐项对拍的基准。刻意「照公式直译」，
// 不要在这里做性能优化。
namespace reference {

bool mse(const ImageBuffer &a, const ImageBuffer &b, double *out,
         std::string *err);

bool ssim(const ImageBuffer &a, const ImageBuffer &b, SsimResult *out,
          std::string *err);

} // namespace reference

} // namespace metrics
} // namespace vulkan
} // namespace image_codec
