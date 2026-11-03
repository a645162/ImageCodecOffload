// File      : jpeg_codec.h
// Brief     : baseline JPEG 编解码门面（CPU 熵编码 + Vulkan DCT/颜色变换）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 分工与 CUDA / OpenCL 后端一致：
//   * 容器（marker / 表 / MCU 布局）与 Huffman 位流在 CPU——位流依赖前一位，
//     串行且不适合 GPU；
//   * DCT/IDCT、颜色变换、色度上下采样在 GPU。
// 解码整条链路在本后端的 GPU 路径里也有 GPU 参与，不存在「解出来了但其实是
// CPU 算的」这种账（gpu_used 会如实回报）。
//
// 通道语义：1（灰度 = 单分量）或 3（RGB = 三分量）。JPEG 没有 Alpha 样本，
// 2/4 通道**显式拒绝**并让路由层回退，绝不降维冒充成功。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"

namespace image_codec {
namespace vulkan {
namespace jpeg {

struct JpegOptions {
  int quality = 90;      // 1..100
  int subsampling = 0;   // 0 = 自动（>=92 用 444，否则 420）；444/420/422 强制
  bool use_gpu = true;   // false = CPU 参考路径（产物与 GPU 逐位相同）
  int device_index = -1; // -1 = 自动选（独显优先）
  size_t memory_limit = 0;
};

// 魔数探测（0xFFD8）。上层据此判断「这张 buffer 是不是 JPEG」，不必先解一次。
inline bool hasJpegSignature(const uint8_t *p, size_t n) {
  return p != nullptr && n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF;
}

// 解码。输出通道数严格跟随 JPEG 分量数：1 分量 -> 1，3 分量 -> 3。
bool decodeJpeg(const uint8_t *data, size_t size, const JpegOptions &options,
                ImageBuffer *out, bool *gpu_used, std::string *err);

// 编码。img.channels 必须是 1 或 3。
bool encodeJpeg(const ImageBuffer &img, const JpegOptions &options,
                std::vector<uint8_t> *out, bool *gpu_used, std::string *err);

} // namespace jpeg
} // namespace vulkan
} // namespace image_codec
