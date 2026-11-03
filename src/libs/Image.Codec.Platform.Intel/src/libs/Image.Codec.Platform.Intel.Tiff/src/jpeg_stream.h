/*
 * File: jpeg_stream.h
 * Brief: Jpeg stream.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// jpeg_stream.h - baseline JPEG 码流的分段解析（为 TIFF JPEG strips 服务）。
//
// TIFF 的 Compression=7 有两种合法写法：
//   1) 每条 strip 是「完整 JPEG 码流」，不写 JPEGTables；
//   2) JPEGTables 存 DQT/DHT，strip 里是「缩写码流」（SOF+SOS+熵数据+EOI）。
//
// libtiff 写出来的是第 2 种，但硬件编码器只会给完整码流，所以这里需要把
// 完整码流拆成「表」与「缩写码流」两半，并顺手把 SOF 里的采样因子读出来
// （YCbCrSubSampling 必须与码流一致，否则解码器会按错误的色度布局解释）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oic {
namespace tiff {

struct JpegComponentInfo {
  uint8_t id = 0;
  uint8_t sampling_h = 1;
  uint8_t sampling_v = 1;
  uint8_t quant_table = 0;
};

// 一条完整 baseline JPEG 码流的解析结果。
struct JpegStreamInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t precision = 8;
  std::vector<JpegComponentInfo> components;
  // JPEGTables 内容：SOI + DQT/DHT + EOI（可直接写进 TIFF 的 347 标签）。
  std::vector<uint8_t> tables;
  // 缩写码流的 header 部分：SOF（以及 DRI 等非表段）。
  std::vector<uint8_t> frame_header;
  // SOS 段（含 FF DA 与长度字段）。
  std::vector<uint8_t> scan_header;
  // 熵编码数据（含重启标记 0xFFDn，不含 EOI）。
  std::vector<uint8_t> entropy;
};

// 解析完整 baseline JPEG。progressive / 算术编码直接拒绝：
// TIFF 6.0 的 JPEG 压缩只定义了 baseline 顺序编码，勉强写进去只会得到
// 一个「别的软件打不开」的文件。
bool parse_jpeg_stream(const uint8_t *data, size_t size, JpegStreamInfo &info,
                       std::string &error);

// 由解析结果拼出缩写码流：frame_header + scan_header + entropy + EOI。
bool make_abbreviated_jpeg(const JpegStreamInfo &info,
                           std::vector<uint8_t> &out, std::string &error);

} // namespace tiff
} // namespace oic
