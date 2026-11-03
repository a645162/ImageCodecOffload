// File      : jpeg_dct_vk.h
// Brief     : JPEG 的 GPU DCT/颜色变换（Vulkan compute）门面
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 分工与 CUDA / OpenCL 后端完全一致：熵编码（Huffman）与容器在 CPU，
// DCT/IDCT、颜色变换、色度下采样/上采样在 GPU。没有把整条链路塞进 GPU，
// 因为 Huffman 的位流依赖前一位，串行且不适合 GPU；这一点三个后端的说法
// 必须一样，不能某一边悄悄换一种分工却还声称「同一张图同一组数字」。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "jpeg_bitstream.h"

namespace image_codec {
namespace vulkan {
namespace jpeg {

// DCT 路径的公共选项。
struct DctOptions {
  bool use_gpu = true;   // false 时由调用方走 CPU 参考路径（本模块不处理）
  int device_index = -1; // -1 = 自动选（独显优先）
  size_t memory_limit = 0;
};

// 解码：量化系数（自然序，布局同 img）-> top-down 像素。
// 输出通道数严格跟随 img.ncomp：1 分量 -> 1 通道，3 分量 -> RGB。
bool gpuDecode(const JpegImage &img, const std::vector<float> &coeffs,
               const DctOptions &options, std::vector<uint8_t> *out,
               std::string *err);

// 编码：top-down 像素 -> 量化系数（自然序，布局同 img，float 整数）。
// 输入通道数严格跟随 img.ncomp：1 分量 -> 灰度（Y 平面原样），3 分量 -> RGB。
bool gpuEncode(const JpegImage &img, const uint8_t *pixels,
               const DctOptions &options, std::vector<float> *coeffs,
               std::string *err);

// 64x64 DCT 基矩阵（含 1/4 归一化与 C(u)C(v)）：m[p][j]，
// p = v*8+u（自然序系数），j = y*8+x（自然序像素）。
// 编码、解码与 CPU 参考共用同一张表——各算一份就等于各有一个舍入差。
void buildDctMatrix(float m[64][64]);

} // namespace jpeg
} // namespace vulkan
} // namespace image_codec
