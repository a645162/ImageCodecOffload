/*
 * File: lzw_codec.h
 * Brief: Lzw codec.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/lzw_codec.h - 统一编解码入口（encode 选 GPU/CPU，decode 选 CPU）。
#pragma once

#include <cstdint>
#include <vector>

namespace image_codec {
namespace tiff {

// 把 top-down 图像写成 TIFF 字节流（不落盘；C API 的 buffer 路径走这个）。
// 返回 0 成功，负值失败。
int tiffEncodeToBuffer(int width, int height, int channels,
                       const uint8_t *pixels, int compression,
                       int rows_per_strip, int backend,
                       std::vector<uint8_t> &out);

// 把 top-down 图像写为 TIFF 文件。
// channels: 1 = 灰度（Photometric=BlackIsZero）、2 = 灰度+Alpha、3 = RGB、
// 4 = RGBA（后两者 Photometric=RGB）。**通道保真**：1 通道进必须 1 通道出，
// 4 通道进的 Alpha 必须原样写出，绝不在这里升维或丢样本。
// compression: 1=none, 5=LZW。backend: 0=OpenCL(GPU), 2=CPU。
// LZW 时 rows_per_strip 会被自动调整为 height 的最大 ≤rps 因子（保证 strip 等长）。
// 返回 0 成功，负值失败。
int tiffEncodeToFile(const char *path, int width, int height, int channels,
                     const uint8_t *pixels, int compression, int rows_per_strip,
                     int backend);

// 读 TIFF 文件并解出 top-down 像素（8-bit 非平面，spp 为 1..4）。
// channels 返回实际通道数。返回 0 成功，负值失败。
int tiffDecodeFromFile(const char *path, int &width, int &height, int &channels,
                       std::vector<uint8_t> &pixels);

// 同上，输入是内存中的完整 TIFF 字节流。
int tiffDecodeFromBuffer(const uint8_t *data, size_t size, int &width,
                         int &height, int &channels,
                         std::vector<uint8_t> &pixels);

// CPU LZW 编码单段（供 backend=CPU 与测试复用；与 GPU kernel 逻辑一致）。
int cpuLzwEncode(const uint8_t *in, size_t in_size, std::vector<uint8_t> &out);

} // namespace tiff
} // namespace image_codec
