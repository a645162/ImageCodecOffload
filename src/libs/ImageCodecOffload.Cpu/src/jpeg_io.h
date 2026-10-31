// jpeg_io.h - baseline JPEG 的 CPU 基线实现（libjpeg-turbo）。
//
// 与 OpenCL / CUDA 后端一致：只做 baseline（SOF0），不支持渐进式编码；
// 解码能读渐进式（libjpeg 本来就支持）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "image_codec_types.h"

namespace image_codec {
namespace offload {
namespace cpu {

Result<std::vector<uint8_t>> encodeJpeg(const Image& image,
                                        const EncodeParams& params);

Result<Image> decodeJpeg(const uint8_t* data, size_t size);

// 只读 SOF 标记返回分量数：1 = 灰度，3 = 彩色。
// 不做像素解码，1 亿像素的图也是微秒级。
Result<int> probeJpegChannels(const uint8_t* data, size_t size);

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
