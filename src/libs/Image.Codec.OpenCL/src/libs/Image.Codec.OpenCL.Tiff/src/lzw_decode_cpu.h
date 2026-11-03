/*
 * File: lzw_decode_cpu.h
 * Brief: Lzw decode cpu.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/lzw_decode_cpu.h - CPU LZW 解码（对齐 libtiff 编码器语义）。
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace image_codec {
namespace tiff {

// 解码一段 LZW 压缩数据。
// 语义：码宽 9->10->11->12 于 next_code==512/1024/2048 切换（与 GPU 编码 kernel 一致），
//       满字典(next_code==4096)后停止加表、收到 ClearCode(256) 重置，EOI(257) 结束。
// 输入不足/非法码返回负值；成功返回 0，out 为解压后的全部字节。
int lzwDecode(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out);

}  // namespace tiff
}  // namespace image_codec
