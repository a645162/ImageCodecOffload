/*
 * File: metrics.h
 * Brief: Metrics.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// metrics.h - 画质指标的 CPU 参考实现（Core 层，纯 C++）。
//
// 三个 GPU 后端各自实现了同一套公式（见各子项目 docs/metrics.md），本文件是
// 它们的共同基准，也是 CPU 回退时的实际实现。常数、窗、边界策略、金字塔方式
// 与子项目里的 metrics_common.h 逐字同源——改一处必须同步改其它处，
// tests/metrics_golden.h 会拦下漂移。
//
// 这里的写法刻意保持「照公式直译」：不做 SIMD、不做循环展开、不做累加顺序
// 优化，也**不允许**把多个样本的累加重新结合成并行归约（(a+b)+(c+d) 与
// ((a+b)+c)+d 在 double 下不是同一个数）。大图 SSIM 慢到分钟级，但并行只发生
// 在「逐样本算窗口统计」这一段：每段结果先写进 band 缓冲，再由单线程按原来的
// 样本顺序累加。因此线程数取 1 还是 64，结果逐位相同，golden 常量不受影响。
#pragma once

#include "image_codec_types.h"

namespace image_codec {
namespace offload {
namespace metrics {

// ---- SSIM 常数（Wang 等 2004，L = 255）----
// 11x11 高斯窗，sigma = 1.5，复制边缘（replicate）边界。
constexpr int kWindow = 11;
constexpr int kHalo = kWindow / 2;
constexpr double kSigma = 1.5;
constexpr double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
constexpr double kC2 = (0.03 * 255.0) * (0.03 * 255.0);

// ---- MS-SSIM 尺度权重（Wang 等 2003）----
constexpr int kMsSsimMaxLevels = 5;
constexpr double kMsSsimWeights[kMsSsimMaxLevels] = {0.0448, 0.2856, 0.3001,
                                                     0.2363, 0.1333};

// 归一化后的 11 点一维高斯权重（二维窗 = 外积）。
void gaussianWeights11(double *out);

// MSE -> PSNR。MSE == 0 固定返回 99.0：有限值，避免 Inf/NaN 污染平均值与 JSON。
double psnrFromMse(double mse);

// MS-SSIM 实际可用的尺度数：每一级仍需 >= 11x11 才能算 11x11 窗。
int msSsimLevels(int width, int height);

// ---- 三个独立入口 ----
// 为什么不合成一个 compare()：11x11 窗的内存流量是纯平方误差的上百倍，
// 只想筛掉编码器坏输出的调用方不该白付这笔钱。SSIM 与 MS-SSIM 合并，
// 是因为 MS-SSIM 的第 0 级就是 SSIM 的那一次统计。
Result<double> mse(const Image &a, const Image &b);
Result<double> psnr(const Image &a, const Image &b);
Result<SsimResult> ssim(const Image &a, const Image &b);

} // namespace metrics
} // namespace offload
} // namespace image_codec
