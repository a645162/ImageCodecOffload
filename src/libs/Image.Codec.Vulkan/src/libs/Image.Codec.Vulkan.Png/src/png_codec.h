// File      : png_codec.h
// Brief     : PNG 编解码门面（编码行滤波在 GPU，deflate/inflate 在 zlib）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"

namespace image_codec {
namespace vulkan {
namespace png {

struct PngEncodeOptions {
  int compression_level = 6; // zlib 0..9
  int filter = -1;           // -1 自适应，0..4 固定
  bool use_gpu = true;       // false 时用容器层的 CPU 参考滤波
  // 每 band 行数（显存占用只与它有关，与图像高度无关）。
  int band_rows = 256;
  // 显存上限（0 = 不限制）。
  size_t memory_limit = 0;
  // 设备选择：-1 = 自动（独显优先），>=0 = 指定下标。
  int device_index = -1;
  // GPU 失败（设备不可用 / 显存不足）时是否回退 CPU 滤波（默认 true：
  // 产出合法 PNG 优先）。回退时 gpu_used 报 false，err 里必须能看到原因。
  bool allow_cpu_fallback = true;
};

// 编码。gpu_used（可为 nullptr）回报本次滤波是否真的在 GPU 上完成。
bool encodePng(const ImageBuffer &img, const PngEncodeOptions &options,
               std::vector<uint8_t> *out, bool *gpu_used, std::string *err);

// 解码：zlib inflate + unfilter 全部在 CPU（依赖链只能按行串行）。
bool decodePng(const uint8_t *data, size_t size, ImageBuffer *out,
               std::string *err);

} // namespace png
} // namespace vulkan
} // namespace image_codec
