/*
 * File: png_codec.h
 * Brief: Png codec.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// png_codec.h - PNG 编解码门面（GPU 行滤波 + zlib）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"

namespace image_codec {
namespace ocl {
namespace png {

struct PngEncodeOptions {
  bool use_gpu = true;       // 是否把行滤波交给 OpenCL
  int filter = -1;           // -1 = 自适应，0..4 = 固定滤波器
  int compression_level = 6; // 0..9
  int block_threads = 0;     // 0 = 256（裁剪到 [32,256]）
  // GPU 滤波失败时是否允许回退 CPU 参考实现。默认允许，但原因必须写进 err。
  bool allow_cpu_fallback = true;
};

// 编码：img.channels 决定 PNG color type（1 -> 灰度，3 -> RGB）。
bool encodePng(const ImageBuffer &img, const PngEncodeOptions &options,
               std::vector<uint8_t> *out, bool *gpu_used, std::string *err);

// 解码：8-bit 非隔行，color type 0/2/4/6。
bool decodePng(const uint8_t *data, size_t size, ImageBuffer *out,
               std::string *err);

} // namespace png
} // namespace ocl
} // namespace image_codec
