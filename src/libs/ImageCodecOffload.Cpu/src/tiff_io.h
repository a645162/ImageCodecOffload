// tiff_io.h - TIFF 的 CPU 基线实现（libtiff）。
//
// 存在的意义有两个：一是没有可用 GPU 时的兜底，二是给 GPU 后端一个「同样
// 输入应该得到同样像素」的参照。编码支持 strip 与 tiled 两种布局，
// 解码统一走 libtiff 的 RGBA 路径（能处理 tiled / LZW / Deflate / 16-bit），
// 再按原图的 photometric 还原成灰度或 RGB——**通道数不能因为解码器方便
// 就把 1 通道升成 3 通道**。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "image_codec_types.h"

namespace image_codec {
namespace offload {
namespace cpu {

Result<std::vector<uint8_t>> encodeTiff(const Image& image,
                                        const EncodeParams& params);

Result<Image> decodeTiff(const uint8_t* data, size_t size);

// 只读 IFD 的 SamplesPerPixel：<= 1（灰度、调色板、二值）= 1，其余 = 3。
// 走 libtiff 的内存读回调，能正确处理 strip/tiled 与 BigTIFF 头。
Result<int> probeTiffChannels(const uint8_t* data, size_t size);

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
