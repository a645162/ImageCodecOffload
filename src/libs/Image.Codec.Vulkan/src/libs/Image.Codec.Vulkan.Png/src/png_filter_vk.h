// File      : png_filter_vk.h
// Brief     : PNG 行滤波的 Vulkan 实现（自适应选择 + 计算）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 分工（与 CUDA / OpenCL 后端一致）：
//   * 行滤波（filter / 自适应选择）在 GPU —— 逐字节、无跨行依赖，正好是显存带宽活；
//   * deflate/inflate 在 zlib（CPU）——GPU deflate 在这些尺寸上没收益，还会让
//     「码流是否与 libpng 兼容」变成不可验证的问题。
//
// 解码不在 GPU：unfilter 的依赖链是「同一行左侧 → 上一行」，只能按行串行，
// 搬到 GPU 只会白多两次 H2D/D2H。这一点三个后端说法一致，不按后端随意改口。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"
#include "vk_buffer.h"
#include "vk_codec_device.h"
#include "vk_pipeline.h"

namespace image_codec {
namespace vulkan {
namespace png {

struct FilterOptions {
  // 每个 band 的行数。显存占用与 band_rows 成正比、与图像高度无关，
  // 因此 1.9 GB 的 Merge.bmp 与 400x300 的小图走同一条代码路径。
  int band_rows = 256;
  // -1 = 自适应（逐行最小绝对值和），0..4 = 固定滤波类型。
  int filter = -1;
  // 显存上限（0 = 不限制）。超限时明确失败并说明「需要多少 / 上限多少」。
  size_t memory_limit = 0;
  // 设备选择：-1 = 自动（独显优先），>=0 = 指定下标（同一台机器上「跑图形的
  // 独显」与「有媒体引擎的核显」未必是同一块卡，调用方要能指定）。
  int device_index = -1;
};

// 生成完整 filtered 流（每行 1 字节滤波类型 + width*channels 字节数据）。
// 成功返回 true；失败时 err 写明是哪一步（设备/分配/派发/回读）。
bool filterRowsOnGpu(const ImageBuffer &img, const FilterOptions &options,
                     std::vector<uint8_t> *out, std::string *err);

} // namespace png
} // namespace vulkan
} // namespace image_codec
