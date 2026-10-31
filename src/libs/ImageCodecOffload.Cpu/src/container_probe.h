// container_probe.h - 只读容器头判断「这张图本来有多少通道」。
//
// 为什么需要它：某些后端的 C ABI 只有 24-bit RGB 一种形态（in 与 out 都是）。
// 一张灰度 JPEG/TIFF 交给它，解码结果会被悄悄升维成 3 通道、编码则直接失败，
// 而调用方事后完全看不出「通道数为什么变了」。因此必须在**调用之前**就知道
// 这张图本来几通道，才能把请求交给正确的后端。
//
// 代价约束：只读头，不解像素。1.9 GB 的 TIFF 也要能在毫秒级回答。
#pragma once

#include <cstddef>
#include <cstdint>

#include "image_codec_types.h"

namespace image_codec {
namespace offload {
namespace cpu {

// 返回 1（灰度）或 3（彩色/调色板）。
// 无法判断时返回 UnsupportedFormat 的错误，调用方应把它当作「不知道」继续走
// 原路径，而不是据此拒绝请求。
Result<int> probeChannels(Format format, const uint8_t* data, size_t size);

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
