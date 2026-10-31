/*
 * File: png_io.h
 * Brief: Png io.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// png_io.h - PNG 的 CPU 基线实现（libpng）。
//
// 编码用完整 libpng API（需要 compression_level / filter 两个参数，
// 简化 API 给不了）；解码用简化 API（png_image）——解码侧不需要这些控制，
// 简化 API 还顺手处理了 16-bit / 调色板 / alpha。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "image_codec_types.h"

namespace image_codec {
namespace offload {
namespace cpu {

Result<std::vector<uint8_t>> encodePng(const Image& image,
                                       const EncodeParams& params);

Result<Image> decodePng(const uint8_t* data, size_t size);

// 只读 IHDR 的 color type：0/4（灰度、灰度+alpha）= 1，其余按 3 处理。
Result<int> probePngChannels(const uint8_t* data, size_t size);

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
