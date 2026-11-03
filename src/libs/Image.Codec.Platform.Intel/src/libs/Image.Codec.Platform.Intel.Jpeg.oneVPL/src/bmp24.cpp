/*
 * File: bmp24.cpp
 * Brief: Bmp24.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// bmp24.cpp - 24-bit 未压缩 BMP 读写实现。
#include "bmp24.h"

#include <cstdio>
#include <cstring>

namespace oic {
namespace jpeg {
namespace onevpl {

namespace {

#pragma pack(push, 1)
struct BmpFileHeader {
  uint16_t bf_type;
  uint32_t bf_size;
  uint16_t bf_reserved1;
  uint16_t bf_reserved2;
  uint32_t bf_off_bits;
};
struct BmpInfoHeader {
  uint32_t bi_size;
  int32_t bi_width;
  int32_t bi_height;
  uint16_t bi_planes;
  uint16_t bi_bit_count;
  uint32_t bi_compression;
  uint32_t bi_size_image;
  int32_t bi_x_pels_per_meter;
  int32_t bi_y_pels_per_meter;
  uint32_t bi_clr_used;
  uint32_t bi_clr_important;
};
#pragma pack(pop)

bool load_file(const char *path, std::vector<uint8_t> &data,
               std::string &error) {
  FILE *file = std::fopen(path, "rb");
  if (file == nullptr) {
    error = std::string("无法打开 ") + path;
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(file);
    error = std::string("文件为空 ") + path;
    return false;
  }
  data.resize(static_cast<size_t>(size));
  const bool ok = std::fread(data.data(), 1, data.size(), file) == data.size();
  std::fclose(file);
  if (!ok) {
    error = std::string("读取失败 ") + path;
  }
  return ok;
}

} // namespace

bool bmp_load_24(const char *path, uint32_t &width, uint32_t &height,
                 std::vector<uint8_t> &bgr, std::string &error) {
  std::vector<uint8_t> file_data;
  if (!load_file(path, file_data, error)) {
    return false;
  }
  if (file_data.size() < 54) {
    error = "BMP 文件头不完整";
    return false;
  }
  BmpFileHeader header{};
  BmpInfoHeader info{};
  std::memcpy(&header, file_data.data(), sizeof(header));
  std::memcpy(&info, file_data.data() + 14, sizeof(info));
  if (header.bf_type != 0x4D42) { // 'BM'
    error = "不是 BMP 文件";
    return false;
  }
  if (info.bi_bit_count != 24 || info.bi_compression != 0) {
    error = "只支持 24-bit 未压缩 BMP";
    return false;
  }
  if (info.bi_width <= 0 || info.bi_height == 0) {
    error = "BMP 宽高非法";
    return false;
  }
  const uint32_t w = static_cast<uint32_t>(info.bi_width);
  const uint32_t h = static_cast<uint32_t>(info.bi_height < 0 ? -info.bi_height
                                                              : info.bi_height);
  const bool bottom_up = info.bi_height > 0;
  const uint32_t row_stride = ((w * 3 + 3) / 4) * 4;
  if (file_data.size() < static_cast<size_t>(header.bf_off_bits) +
                             static_cast<size_t>(row_stride) * h) {
    error = "BMP 像素数据不完整";
    return false;
  }

  bgr.assign(static_cast<size_t>(w) * h * 3, 0);
  const uint8_t *src = file_data.data() + header.bf_off_bits;
  for (uint32_t y = 0; y < h; ++y) {
    const uint32_t src_row = bottom_up ? (h - 1 - y) : y;
    std::memcpy(bgr.data() + static_cast<size_t>(y) * w * 3,
                src + static_cast<size_t>(src_row) * row_stride,
                static_cast<size_t>(w) * 3);
  }
  width = w;
  height = h;
  return true;
}

bool bmp_save_24(const char *path, const uint8_t *bgr, uint32_t width,
                 uint32_t height, std::string &error) {
  const uint32_t stride = ((width * 3 + 3) / 4) * 4;
  FILE *file = std::fopen(path, "wb");
  if (file == nullptr) {
    error = std::string("无法写入 ") + path;
    return false;
  }
  BmpFileHeader header{};
  BmpInfoHeader info{};
  header.bf_type = 0x4D42;
  header.bf_size = 54 + stride * height;
  header.bf_off_bits = 54;
  info.bi_size = 40;
  info.bi_width = static_cast<int32_t>(width);
  info.bi_height = static_cast<int32_t>(height); // 正数 -> bottom-up
  info.bi_planes = 1;
  info.bi_bit_count = 24;
  info.bi_size_image = stride * height;
  std::fwrite(&header, 1, sizeof(header), file);
  std::fwrite(&info, 1, sizeof(info), file);
  std::vector<uint8_t> row(stride, 0);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *src = bgr + static_cast<size_t>(height - 1 - y) * width * 3;
    std::memcpy(row.data(), src, static_cast<size_t>(width) * 3);
    std::fwrite(row.data(), 1, stride, file);
  }
  std::fclose(file);
  return true;
}

} // namespace onevpl
} // namespace jpeg
} // namespace oic
