// File      : metrics_common.h
// Brief     : MSE / PSNR / SSIM / MS-SSIM 的共享定义（CPU 参考与 GPU 着色器共用）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 为什么常数与公式只写一份：GPU 与 CPU 必须给出**同一个数字**，否则「换了后端
// 画质变了」这种结论根本无法判断。常数、边界策略、金字塔构建方式只要有一处
// 不同，两边的差距就会稳定在 1e-3 量级——肉眼看不出来，横向比较全部作废。
//
// 与 Image.Codec.CUDA / Image.Codec.OpenCL 的定义逐项对齐（改一处必须同步三处）：
//   * SSIM：11x11 高斯窗，σ = 1.5，复制边缘，C1 = (0.01·255)²，C2 = (0.03·255)²；
//   * MS-SSIM：5 级，2x2 均值下采样（floor(w/2)），权重 0.0448 / 0.2856 /
//     0.3001 / 0.2363 / 0.1333，按实际级数重新归一化；
//   * PSNR：10·log10(255²/MSE)，MSE == 0 固定 99.0（避免 Inf 污染平均值）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"

namespace image_codec {
namespace vulkan {
namespace metrics {

// ---- SSIM 常数（Wang 等 2004，L = 255）----
constexpr int kWindow = 11;
constexpr int kHalo = kWindow / 2;
constexpr double kSigma = 1.5;
constexpr double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
constexpr double kC2 = (0.03 * 255.0) * (0.03 * 255.0);

// ---- MS-SSIM 尺度权重（Wang 等 2003）----
constexpr int kMsSsimMaxLevels = 5;
constexpr double kMsSsimWeights[kMsSsimMaxLevels] = {0.0448, 0.2856, 0.3001,
                                                     0.2363, 0.1333};

// 单个尺度的累加量。
struct ScaleStats {
  double sq_sum = 0.0;   // Σ(x-y)²（MSE 用，只有第 0 级有意义）
  double ssim_sum = 0.0; // Σ SSIM(px)
  double cs_sum = 0.0;   // Σ CS(px)（MS-SSIM 中间尺度只用它）
  uint64_t samples = 0;  // 参与统计的样本数（像素 × 通道）
};

// 归一化后的 11 点一维高斯权重（二维窗 = 外积）。
void gaussianWeights11(double *out);

// MSE -> PSNR。
double psnrFromMse(double mse);

// MS-SSIM 实际可用的尺度数（每级仍需 >= 11x11 才能算 11x11 窗）。
int msSsimLevels(int width, int height);

// 2x2 均值下采样（各通道独立）。输出尺寸为 floor(w/2) x floor(h/2)，
// 奇数尾行/尾列被丢弃——与 pytorch-msssim 的 avg_pool2d(kernel=2) 一致。
bool downsample2x(const ImageBuffer &src, ImageBuffer *dst);

// 由各尺度统计合成 MS-SSIM。
//   cs[j]     : 第 j 级（0 基）的 CS 均值，共 levels 个
//   ssim_last : 最末一级的完整 SSIM 均值
double msSsimFromScales(const double *cs, double ssim_last, int levels);

} // namespace metrics
} // namespace vulkan
} // namespace image_codec
