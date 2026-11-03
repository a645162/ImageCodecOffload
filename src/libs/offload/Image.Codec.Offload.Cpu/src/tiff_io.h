/*
 * File: tiff_io.h
 * Brief: Tiff io.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

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
#include "tiff_container.h"

namespace image_codec {
namespace offload {
namespace cpu {

Result<std::vector<uint8_t>> encodeTiff(const Image& image,
                                        const EncodeParams& params);

// 把 [first_row, first_row + rows) 独立压缩成一条 strip 的压缩流（不含容器）。
//
// 结果必须与「同一张图、同样标签、RowsPerStrip = rows 整图编码」时对应那条
// 的压缩流逐字节相同——分片拼装靠的就是这一点。
Result<std::vector<uint8_t>> encodeTiffStrip(const Image& image, int first_row,
                                             int rows,
                                             const EncodeParams& params);

// 由参数推导容器布局（压缩标签映射、DPI 缺省、片高）。
TiffLayout layoutFromParams(const Image& image, const EncodeParams& params,
                            int rows_per_strip);

Result<Image> decodeTiff(const uint8_t* data, size_t size);

// 只读 IFD 的 SamplesPerPixel：<= 1（灰度、调色板、二值）= 1，其余 = 3。
// 走 libtiff 的内存读回调，能正确处理 strip/tiled 与 BigTIFF 头。
Result<int> probeTiffChannels(const uint8_t* data, size_t size);

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
