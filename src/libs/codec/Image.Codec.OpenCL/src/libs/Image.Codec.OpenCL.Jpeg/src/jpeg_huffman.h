/*
 * File: jpeg_huffman.h
 * Brief: Jpeg huffman.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/jpeg_huffman.h - CPU 熵编解码（Huffman）。
// 解码：位流 -> 量化 DCT 系数（zigzag 顺序转自然序，DC 差分+符号扩展+AC RLE）。
// 编码：量化系数 -> 位流（标准表）。
#pragma once

#include "jpeg_bitstream.h"

#include <cstdint>
#include <string>
#include <vector>

namespace image_codec {
namespace jpeg {

// MSB-first 位读取器：处理 0xFF00 去填充、RSTn 标记与 EOI。
class JpegBitReader {
 public:
  JpegBitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  // 读取 n 位（0<=n<=16）。返回 -1 表示遇到 RST/EOI/数据耗尽。
  int get_bits(int n);
  // 丢弃缓冲的填充位并消费下一个 RST 标记。成功返回 true。
  bool consume_restart();

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  uint32_t bitbuf_ = 0;
  int bitcnt_ = 0;
  int restart_marker_ = -1;
  bool eof_ = false;
};

// 把 img.scans 的熵数据解码到组合系数缓冲区（float，自然序，块序 = 行优先）。
// 成功返回 0。coeffs 大小 = img.total_blocks * 64。
int jpeg_entropy_decode(const JpegImage& img, std::vector<float>* coeffs,
                        std::string* err);

// 把组合系数缓冲区编码为熵字节流（已含 0xFF00 字节填充）。成功返回 0。
// 编码使用 img.huff_dc/ac（标准表）。单扫描交错 MCU 序。
int jpeg_entropy_encode(const JpegImage& img, const std::vector<float>& coeffs,
                        std::vector<uint8_t>* entropy, std::string* err);

}  // namespace jpeg
}  // namespace image_codec
