/*
 * File: tiff_jpeg_strip_writer.h
 * Brief: Tiff jpeg strip writer.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// tiff_jpeg_strip_writer.h - 自写 TIFF 6.0 容器，把「预编码的 JPEG strips」
// 直接写进 TIFF（Compression=7），以及无硬件时的 Compression=1 基线。
//
// 为什么不用 libtiff 写：libtiff 的 JPEG codec 会拿 strip 像素重新编码
// （它只暴露「给我像素」的接口），没有任何入口能写进一段已经由硬件编好的
// 码流。于是容器部分自己写 IFD（~200 行），libtiff 只用于**测试端解码校验**。
//
// TIFF JPEG 约定（TIFF 6.0 §21）：
//   Photometric=YCbCr、BitsPerSample=8,8,8、SamplesPerPixel=3、
//   YCbCrSubSampling 必须与 SOF0 里的采样因子一致；
//   JPEGTables 存 DQT/DHT，strip 内是缩写码流（SOI+SOF+SOS+熵数据+EOI）。
// 每条 strip 的高度即它实际覆盖的行数（末条通常短于 RowsPerStrip），
// 编码器给出的 SOF 宽高必须与请求完全一致——libtiff 会拿 SOF 尺寸与
// ImageWidth/ImageLength 推导出的 strip 尺寸比对，补过边的尺寸会被判成
// "strip 比图像还大"。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oic {
namespace tiff {

enum class TiffCompression : uint16_t {
  kNone = 1,
  kJpeg = 7,
};

// 一份已经准备好的 strip 布局（与编码方式无关）。
struct TiffStripImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t rows_per_strip = 0;
  TiffCompression compression = TiffCompression::kNone;
  uint16_t photometric = 2; // 2 = RGB, 6 = YCbCr
  uint16_t subsampling_h = 1;
  uint16_t subsampling_v = 1;
  std::vector<std::vector<uint8_t>> strips;
  // 仅 Compression=7 使用：空表示每条 strip 都是完整 JPEG 码流。
  std::vector<uint8_t> jpeg_tables;
};

// 生成 TIFF 字节流 / 写文件。校验失败时 error 说明原因，不产生半截文件。
bool encode_tiff_strips(const TiffStripImage &image, std::vector<uint8_t> &out,
                        std::string &error);
bool write_tiff_strips(const char *path, const TiffStripImage &image,
                       std::string &error);

// 把一条 strip 的 RGB 像素编码成 JPEG（完整码流）。硬件实现与测试用的
// libjpeg 参考实现都套这个接口，容器代码因此与具体编码器无关。
struct JpegStripEncoder {
  uint32_t quality = 75;
  std::function<bool(const uint8_t *rgb, uint32_t width, uint32_t rows,
                     std::vector<uint8_t> &jpeg, std::string &error)>
      encode;
};

struct JpegStripTiffReport {
  uint32_t strips = 0;
  uint32_t rows_per_strip = 0;
  uint16_t subsampling_h = 1;
  uint16_t subsampling_v = 1;
  bool used_jpeg_tables = true;
  uint32_t encoded_width = 0; // 实际送进编码器的宽度（= ImageWidth）
};

// 高层入口：RGB24 -> 逐 strip 调用 encoder -> Compression=7 的 TIFF。
//
// rows_per_strip 是每条 strip 的行数（末条可以是更少）。
// 编码器返回的 JPEG 宽高必须与请求完全一致（见上面的 SOF 尺寸说明）。
bool write_tiff_jpeg_strips(const char *path, const uint8_t *rgb,
                            uint32_t width, uint32_t height,
                            uint32_t rows_per_strip,
                            const JpegStripEncoder &encoder,
                            JpegStripTiffReport &report, std::string &error);

// 无硬件时的基线：RGB24 -> Compression=1 的 TIFF（不压缩）。
bool write_tiff_rgb_none(const char *path, const uint8_t *rgb, uint32_t width,
                         uint32_t height, uint32_t rows_per_strip,
                         std::string &error);

} // namespace tiff
} // namespace oic
