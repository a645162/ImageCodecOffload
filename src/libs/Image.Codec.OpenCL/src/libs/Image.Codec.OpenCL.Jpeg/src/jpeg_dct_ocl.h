/*
 * File: jpeg_dct_ocl.h
 * Brief: Jpeg dct ocl.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/jpeg_dct_ocl.h - GPU DCT/颜色变换（OpenCL）与纯 CPU 参考路径。
// 解码：量化系数 -> GPU(反量化+IDCT+YCbCr->RGB+上采样) -> RGB。
// 编码：RGB -> GPU(RGB->YCbCr+下采样+FDCT+量化) -> 量化系数。
#pragma once

#include "jpeg_bitstream.h"

#include <cstdint>
#include <string>
#include <vector>

namespace image_codec {
namespace jpeg {

// 内嵌 kernel 源码（由 CMake 生成的 jpeg_cl_strings.cpp 提供）。
extern const char kJpegDecSource[];
extern const char kJpegEncSource[];

// GPU 解码：coeffs（自然序，布局同 img）-> top-down 像素。
// 输出通道数跟随 img.ncomp：1 分量 -> 1 通道，3 分量 -> RGB。
// 成功返回 0。需 OpenCL 设备可用。
int jpeg_gpu_decode(const JpegImage &img, const std::vector<float> &coeffs,
                    std::vector<uint8_t> *rgb, std::string *err);

// GPU 编码：top-down 像素 -> 量化系数（自然序，布局同 img，float 整数）。
// 输入通道数跟随 img.ncomp：1 分量 -> 灰度（Y 平面原样），3 分量 -> RGB->YCbCr。
// 成功返回 0。
int jpeg_gpu_encode(const JpegImage &img, const uint8_t *rgb,
                    std::vector<float> *coeffs, std::string *err);

// 纯 CPU 解码（浮点 IDCT，验证 GPU 路径用；不依赖 OpenCL）。
int jpeg_cpu_decode(const JpegImage &img, const std::vector<float> &coeffs,
                    std::vector<uint8_t> *rgb, std::string *err);

} // namespace jpeg
} // namespace image_codec
