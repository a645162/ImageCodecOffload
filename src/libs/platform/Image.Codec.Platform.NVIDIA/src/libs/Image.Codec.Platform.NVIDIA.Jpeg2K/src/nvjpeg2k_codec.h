/*
 * File: nvjpeg2k_codec.h
 * Brief: JPEG 2000 codec on nvJPEG2000.
 * Author: Haomin Kong
 * Email: konghm@beim.com.cn / a645162@163.com
 * Created: 2026-11-03
 */

// nvjpeg2k_codec.h - JPEG 2000 / JP2 编解码（nvJPEG2000）。
//
// 与 CPU 基线（OpenJPEG）的取舍一致：默认写 **JP2 容器**（与 OpenJPEG 的
// opj_create_compress(OPJ_CODEC_JP2) 对齐），解码同时接受裸码流 (.j2k) 与 JP2。
//
// 8 bit/样本、无符号、1 或 3 通道：这是本项目图像模型（1/3/4 通道 8 bit）能
// 表达的范围。超出范围时返回 false + 具体原因，由上层回退 CPU，绝不偷偷降位深。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"
#include "nvidia_runtime.h"

namespace image_codec {
namespace nvidia {
namespace jpeg2k {

// 本编译单元是否链接了 nvJPEG2000。
bool available();

// nvJPEG2000 库版本（"0.12"）；不可用时返回空串。
std::string version();

// 解码。灰度返回 1 通道，彩色返回 3 通道；位深 <= 8 时直通，9..16 位按
// 精度右移到 8 bit（保持视觉范围），有符号分量不支持。
bool decode(int device_index, const uint8_t *data, size_t size,
            ImageBuffer *out, std::string *err, MemoryBudget *budget = nullptr);

// 编码。quality == 0 表示无损（可逆 5/3）；1..100 走有损（不可逆 9/7）+
// JPEG 风格质量因子。channels 支持 1（灰度）与 3（RGB）。
bool encode(int device_index, const ImageBuffer &img, int quality,
            std::vector<uint8_t> *out, std::string *err,
            MemoryBudget *budget = nullptr);

// 魔数：裸码流 SOC (FF 4F) 或 JP2 签名盒 (00 00 00 0C 6A 50 20 20)。
inline bool has_signature(const uint8_t *p, size_t n) {
  if (p == nullptr) {
    return false;
  }
  if (n >= 2 && p[0] == 0xFF && p[1] == 0x4F) {
    return true;
  }
  static const uint8_t k_jp2[8] = {0x00, 0x00, 0x00, 0x0C,
                                   0x6A, 0x50, 0x20, 0x20};
  if (n < sizeof(k_jp2)) {
    return false;
  }
  for (size_t i = 0; i < sizeof(k_jp2); ++i) {
    if (p[i] != k_jp2[i]) {
      return false;
    }
  }
  return true;
}

} // namespace jpeg2k
} // namespace nvidia
} // namespace image_codec
