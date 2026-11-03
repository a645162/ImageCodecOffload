/*
 * File: jpeg_codec.h
 * Brief: Jpeg codec.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/jpeg_codec.h - JPEG 混合编解码编排（读/写文件、CPU Huffman + GPU DCT）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace image_codec {
namespace jpeg {

struct JpegDecodeResult {
  int width = 0;
  int height = 0;
  int ncomp = 0;
  int channels = 3;         // 1 = 灰度，3 = RGB；与 ncomp 一致（通道保真）
  int subsampling = 444;    // 由采样因子推断
  std::vector<uint8_t> rgb; // top-down，width*height*channels
  std::string error;
};

// 解码 JPEG 字节流。backend: 0=OpenCL，1=纯 CPU（参考）。成功返回 0。
int jpeg_decode_buffer(const std::vector<uint8_t> &file, int backend,
                       JpegDecodeResult *out);

// 编码 top-down 像素为 JPEG 字节流。quality 1..100。
// channels: 1 = 灰度（单分量，4:4:4），3 = RGB。1 通道进必须 1 分量出。
// subsampling: 444/420/422（>0 时强制；0 时按质量自动：>=92 用 444，否则 420）；
//              灰度恒为 444（没有色度可下采样）。
// backend: 0=OpenCL（当前唯一实现）。成功返回 0。
int jpeg_encode_buffer(const std::vector<uint8_t> &pixels, int width,
                       int height, int channels, int quality, int subsampling,
                       int backend, std::vector<uint8_t> *jpeg_out,
                       std::string *err);

// 便捷：读 BMP -> 解码/编码路径由上层组合。
int jpeg_decode_file(const std::string &in_path, int backend,
                     JpegDecodeResult *out);
int jpeg_encode_bmp_file(const std::string &in_bmp, const std::string &out_jpg,
                         int backend, int quality, int subsampling,
                         std::string *err);

} // namespace jpeg
} // namespace image_codec
