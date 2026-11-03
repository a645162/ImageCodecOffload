// File      : jpeg_dct_cpu.h
// Brief     : JPEG 的纯 CPU 参考实现（IDCT/FDCT/颜色变换/上下采样）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 为什么 Vulkan 后端里还留一份 CPU DCT：它**不是**兜底手段，而是自测的对照物。
// GPU 路径算出来的像素只有和同仓库的 CPU 逐位比过，才能说「这条着色器写对了」；
// 顺便也让 use_gpu=false 成为一个真实可用的选项（例如设备被独占或太慢时）。
//
// 两边必须逐位一致：同一张基矩阵、同一累加顺序（k = 0..63）、同一舍入
// （`floor(x + 0.5)`，不用 round——GLSL 的半值方向未规定）。任何一处漂移都会
// 让「GPU vs CPU」这条回归线失效。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jpeg_bitstream.h"

namespace image_codec {
namespace vulkan {
namespace jpeg {

// 量化系数 -> top-down 像素（1 分量 -> 1 通道，3 分量 -> RGB）。
bool cpuDecode(const JpegImage &img, const std::vector<float> &coeffs,
               std::vector<uint8_t> *out, std::string *err);

// top-down 像素 -> 量化系数（1 分量 -> 灰度，3 分量 -> RGB）。
bool cpuEncode(const JpegImage &img, const uint8_t *pixels,
               std::vector<float> *coeffs, std::string *err);

} // namespace jpeg
} // namespace vulkan
} // namespace image_codec
