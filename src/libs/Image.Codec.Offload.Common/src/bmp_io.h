/*
 * File: bmp_io.h
 * Brief: Bmp io.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// bmp_io.h - BMP 读写（纯 CPU，仅用于 CLI 与测试的中转与比对）。
//
// 为什么放在 Common 而不是各模块里：三个后端各自都有一份只服务于自测的
// bmp_io，但路由层需要一个「喂给编码器 / 从解码器拿出来看」的通用格式，
// 而 BMP 是唯一不带压缩语义、用来看像素对不对的格式。
//
// 不承诺写出的 BMP 能被所有软件打开：只覆盖本库实际用到的三种
// （8-bit 灰度调色板、24-bit BGR、32-bit BGRA）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "image_codec_types.h"

namespace image_codec {
namespace offload {
namespace common {

// BMP 头里的几何信息。只读前 54 字节即可取得，用于 1.9 GB 级大图的
// `ico info`（不能为了打印尺寸把整幅图读进内存）。
struct BmpHeader {
  int width = 0;
  int height = 0;
  int channels = 0; // 8-bit -> 1；24-bit -> 3；32-bit -> 4（带 alpha）
};

Result<BmpHeader> probeBmp(const uint8_t *data, size_t size);

// 读 BMP。支持 8-bit（调色板灰度为 1 通道）、24-bit、32-bit（输出 4 通道，
// BI_BITFIELDS 采用文件里的 Alpha；BI_RGB 的保留字节按不透明处理）。
// top-down 与 bottom-up、行尾 4 字节对齐都处理。
Result<Image> readBmp(const uint8_t *data, size_t size);

// 写 BMP。channels == 1 写 8-bit 调色板灰度；3 写 24-bit BGR；
// 4 写 32-bit BI_BITFIELDS（RGBA 掩码，Alpha 原样写出）。
// 2 通道（灰度+Alpha）在 BMP 里没有表示法，明确返回 UnsupportedFormat。
Result<std::vector<uint8_t>> writeBmp(const Image &image);

} // namespace common
} // namespace offload
} // namespace image_codec
