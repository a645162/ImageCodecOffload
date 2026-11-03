/*
 * File: nvtiff_codec.h
 * Brief: TIFF codec on nvTIFF.
 * Author: Haomin Kong
 * Email: konghm@beim.com.cn / a645162@163.com
 * Created: 2026-11-03
 */

// nvtiff_codec.h - TIFF 编解码（nvTIFF）。
//
// 与 Image.Codec.CUDA / OpenCL / Vulkan 的 TIFF 不同的地方：那三个后端自己写
// 容器、只把 LZW / 预测器 / 行滤波搬到 GPU；nvTIFF 是**整条容器 + 压缩**都在
// 厂商库里（LZW / Deflate / JPEG / PackBits / JPEG2000 都由它写）。
//
// 8 / 16 bit 无符号、1..4 通道；16 bit 解码后按精度右移到 8 bit 输出。
// 超出范围返回 false + 具体原因，由上层回退 CPU。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"
#include "nvidia_runtime.h"

namespace image_codec {
namespace nvidia {
namespace tiff {

// 本编译单元是否链接了 nvTIFF。
bool available();

// nvTIFF 库版本（"0.9"）；不可用时返回空串。
std::string version();

// 编码参数。compression 取 TIFF Compression 标签值：
//   1 = NONE、5 = LZW、7 = JPEG、8 = Adobe Deflate、32773 = PackBits。
// rows_per_strip == 0 表示按图像高度自适应（至多 256 行/片）。
struct EncodeOptions {
  int compression = 5;
  int rows_per_strip = 0;
  int jpeg_quality = 90;
};

// 解码。通道数 = 文件里的 SamplesPerPixel（1..4），保持通道保真。
bool decode(int device_index, const uint8_t *data, size_t size,
            ImageBuffer *out, std::string *err, MemoryBudget *budget = nullptr);

// 编码。channels 支持 1（灰度 / MINISBLACK）与 3（RGB）。
bool encode(int device_index, const ImageBuffer &img,
            const EncodeOptions &options, std::vector<uint8_t> *out,
            std::string *err, MemoryBudget *budget = nullptr);

// 魔数：II*\0（小端）或 MM\0*（大端）；BigTIFF 依次为 II+\0 / MM\0+。
inline bool has_signature(const uint8_t *p, size_t n) {
  if (p == nullptr || n < 4) {
    return false;
  }
  const bool little = p[0] == 'I' && p[1] == 'I';
  const bool big = p[0] == 'M' && p[1] == 'M';
  if (!little && !big) {
    return false;
  }
  // 字节序决定魔数落在哪两个字节：II 为 <42 00>（BigTIFF <43 00>），
  // MM 为 <00 42>（BigTIFF <00 43>）。
  const uint8_t lo = little ? p[2] : p[3];
  const uint8_t hi = little ? p[3] : p[2];
  return hi == 0u && (lo == 42u || lo == 43u);
}

} // namespace tiff
} // namespace nvidia
} // namespace image_codec
