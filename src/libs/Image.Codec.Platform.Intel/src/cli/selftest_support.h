/*
 * File: selftest_support.h
 * Brief: Selftest support.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// selftest_support.h - 自测用的参考实现（libjpeg 编码 / libtiff 解码）。
//
// 它们只服务 CLI 自测，不进库：
//  - libjpeg 提供「软件 JPEG strip 编码器」，让 TIFF 容器在没有 Intel 媒体
//    引擎的机器上也能被完整验证（硬件路径与容器路径互不依赖）；
//  - libtiff 只用于解码校验，绝不用于写文件（它会把 JPEG strip 重新编码）。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oic {
namespace selftest {

// 是否编译进了可用的参考实现。
bool have_jpeg_encoder();
bool have_tiff_decoder();

// 软件 JPEG 编码（complete baseline 码流，4:2:0）。
bool encode_jpeg_reference(const uint8_t *rgb, uint32_t width, uint32_t rows,
                           uint32_t quality, std::vector<uint8_t> &jpeg,
                           std::string &error);

// 用 libtiff 解码内存中的 TIFF -> 紧致 RGB24。只验证容器：
// JPEG 压缩的 TIFF 会被 libtiff 解码并完成 YCbCr->RGB 转换。
bool decode_tiff_reference(const std::vector<uint8_t> &tiff, uint32_t &width,
                           uint32_t &height, std::vector<uint8_t> &rgb,
                           std::string &error);

// 生成测试图像（RGB24 渐变 + 方块，覆盖奇数尺寸与色度边界）。
std::vector<uint8_t> make_test_image(uint32_t width, uint32_t height);

// 计算 PSNR（分贝）；完全一致时返回 999。
double psnr_rgb(const uint8_t *a, const uint8_t *b, size_t bytes);

} // namespace selftest
} // namespace oic
