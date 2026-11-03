/*
 * File: tiff_jpeg_strip_writer.cpp
 * Brief: Tiff jpeg strip writer.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// tiff_jpeg_strip_writer.cpp - 自写 TIFF 6.0 容器（Compression=1 / 7）。
#include "tiff_jpeg_strip_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "jpeg_stream.h"

namespace oic {
namespace tiff {

namespace {

constexpr uint16_t kTagImageWidth = 256;
constexpr uint16_t kTagImageLength = 257;
constexpr uint16_t kTagBitsPerSample = 258;
constexpr uint16_t kTagCompression = 259;
constexpr uint16_t kTagPhotometric = 262;
constexpr uint16_t kTagStripOffsets = 273;
constexpr uint16_t kTagSamplesPerPixel = 277;
constexpr uint16_t kTagRowsPerStrip = 278;
constexpr uint16_t kTagStripByteCounts = 279;
constexpr uint16_t kTagXResolution = 282;
constexpr uint16_t kTagYResolution = 283;
constexpr uint16_t kTagPlanarConfig = 284;
constexpr uint16_t kTagResolutionUnit = 296;
constexpr uint16_t kTagJpegTables = 347;
constexpr uint16_t kTagYCbCrSubSampling = 530;
constexpr uint16_t kTagReferenceBlackWhite = 532;

constexpr uint16_t kTypeShort = 3;
constexpr uint16_t kTypeLong = 4;
constexpr uint16_t kTypeRational = 5;
constexpr uint16_t kTypeUndefined = 7;

void append_u16(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void append_u32(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void put_u32(std::vector<uint8_t> &out, size_t offset, uint32_t value) {
  out[offset + 0] = static_cast<uint8_t>(value & 0xFF);
  out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void align_to(std::vector<uint8_t> &out, size_t alignment) {
  while ((out.size() % alignment) != 0) {
    out.push_back(0);
  }
}

std::vector<uint8_t> encode_u16_list(const std::vector<uint16_t> &values) {
  std::vector<uint8_t> bytes;
  bytes.reserve(values.size() * 2);
  for (uint16_t value : values) {
    append_u16(bytes, value);
  }
  return bytes;
}

std::vector<uint8_t> encode_u32_list(const std::vector<uint32_t> &values) {
  std::vector<uint8_t> bytes;
  bytes.reserve(values.size() * 4);
  for (uint32_t value : values) {
    append_u32(bytes, value);
  }
  return bytes;
}

std::vector<uint8_t> encode_rational(uint32_t numerator, uint32_t denominator) {
  std::vector<uint8_t> bytes;
  append_u32(bytes, numerator);
  append_u32(bytes, denominator);
  return bytes;
}

struct IfdEntry {
  uint16_t tag = 0;
  uint16_t type = 0;
  uint32_t count = 0;
  std::vector<uint8_t> value; // 已按 TIFF 类型编码的原始字节
  bool out_of_line = false;
  uint32_t value_offset = 0;
};

void add_entry(std::vector<IfdEntry> &entries, uint16_t tag, uint16_t type,
               uint32_t count, std::vector<uint8_t> value) {
  IfdEntry entry;
  entry.tag = tag;
  entry.type = type;
  entry.count = count;
  entry.value = std::move(value);
  entry.out_of_line = entry.value.size() > 4;
  entries.push_back(std::move(entry));
}

void add_short_entry(std::vector<IfdEntry> &entries, uint16_t tag,
                     uint16_t value) {
  std::vector<uint8_t> bytes;
  append_u16(bytes, value);
  add_entry(entries, tag, kTypeShort, 1, std::move(bytes));
}

void add_long_entry(std::vector<IfdEntry> &entries, uint16_t tag,
                    uint32_t value) {
  std::vector<uint8_t> bytes;
  append_u32(bytes, value);
  add_entry(entries, tag, kTypeLong, 1, std::move(bytes));
}

} // namespace

bool encode_tiff_strips(const TiffStripImage &image, std::vector<uint8_t> &out,
                        std::string &error) {
  out.clear();
  error.clear();
  if (image.width == 0 || image.height == 0) {
    error = "TIFF 宽高不能为 0";
    return false;
  }
  if (image.rows_per_strip == 0) {
    error = "RowsPerStrip 不能为 0";
    return false;
  }
  const uint32_t expected_strips =
      (image.height + image.rows_per_strip - 1) / image.rows_per_strip;
  if (image.strips.size() != expected_strips) {
    error = "strip 数量与几何不符：期望 " + std::to_string(expected_strips) +
            " 条，实际 " + std::to_string(image.strips.size()) + " 条";
    return false;
  }
  if (image.compression == TiffCompression::kJpeg) {
    if (image.photometric != 6) {
      error = "JPEG 压缩的 TIFF 必须是 YCbCr（Photometric=6）";
      return false;
    }
    if (image.subsampling_h == 0 || image.subsampling_v == 0) {
      error = "YCbCrSubSampling 不能为 0";
      return false;
    }
  } else if (image.compression == TiffCompression::kNone) {
    if (image.photometric != 2) {
      error = "未压缩 TIFF 必须是 RGB（Photometric=2）";
      return false;
    }
    const size_t bytes_per_row = static_cast<size_t>(image.width) * 3;
    size_t total = 0;
    for (const auto &strip : image.strips) {
      total += strip.size();
    }
    if (total != bytes_per_row * image.height) {
      error = "未压缩 strip 总字节数与几何不符";
      return false;
    }
  } else {
    error = "不支持的 TIFF 压缩方式";
    return false;
  }

  // ---- header ----
  out.push_back('I');
  out.push_back('I');
  append_u16(out, 42); // magic
  const size_t ifd_pointer_pos = out.size();
  append_u32(out, 0); // 稍后回填 IFD 偏移

  // ---- strip 数据 ----
  std::vector<uint32_t> strip_offsets;
  std::vector<uint32_t> strip_counts;
  strip_offsets.reserve(image.strips.size());
  strip_counts.reserve(image.strips.size());
  for (const auto &strip : image.strips) {
    align_to(out, 2);
    strip_offsets.push_back(static_cast<uint32_t>(out.size()));
    strip_counts.push_back(static_cast<uint32_t>(strip.size()));
    out.insert(out.end(), strip.begin(), strip.end());
  }

  // ---- 附加数据（IFD 之前）----
  std::vector<IfdEntry> entries;
  add_long_entry(entries, kTagImageWidth, image.width);
  add_long_entry(entries, kTagImageLength, image.height);
  add_entry(entries, kTagBitsPerSample, kTypeShort, 3,
            encode_u16_list({8, 8, 8}));
  add_short_entry(entries, kTagCompression,
                  static_cast<uint16_t>(image.compression));
  add_short_entry(entries, kTagPhotometric, image.photometric);
  add_entry(entries, kTagStripOffsets, kTypeLong,
            static_cast<uint32_t>(strip_offsets.size()),
            encode_u32_list(strip_offsets));
  add_short_entry(entries, kTagSamplesPerPixel, 3);
  add_long_entry(entries, kTagRowsPerStrip,
                 image.rows_per_strip < image.height ? image.rows_per_strip
                                                     : image.height);
  add_entry(entries, kTagStripByteCounts, kTypeLong,
            static_cast<uint32_t>(strip_counts.size()),
            encode_u32_list(strip_counts));
  add_entry(entries, kTagXResolution, kTypeRational, 1, encode_rational(72, 1));
  add_entry(entries, kTagYResolution, kTypeRational, 1, encode_rational(72, 1));
  add_short_entry(entries, kTagPlanarConfig, 1);
  add_short_entry(entries, kTagResolutionUnit, 2); // inch
  if (image.compression == TiffCompression::kJpeg) {
    add_entry(entries, kTagYCbCrSubSampling, kTypeShort, 2,
              encode_u16_list({image.subsampling_h, image.subsampling_v}));
    // TIFF 6.0 §21 要求 YCbCr 图像给出 ReferenceBlackWhite；
    // 缺省值虽然是 (0,255,128,255,128,255)，但显式写出来才能让
    // 「按缺省值解释」的实现与「按标签解释」的实现得到同一个结果。
    std::vector<uint8_t> reference;
    append_u32(reference, 0);
    append_u32(reference, 1);
    append_u32(reference, 255);
    append_u32(reference, 1);
    append_u32(reference, 128);
    append_u32(reference, 1);
    append_u32(reference, 255);
    append_u32(reference, 1);
    append_u32(reference, 128);
    append_u32(reference, 1);
    append_u32(reference, 255);
    append_u32(reference, 1);
    add_entry(entries, kTagReferenceBlackWhite, kTypeRational, 6,
              std::move(reference));
    if (!image.jpeg_tables.empty()) {
      add_entry(entries, kTagJpegTables, kTypeUndefined,
                static_cast<uint32_t>(image.jpeg_tables.size()),
                image.jpeg_tables);
    }
  }

  // 大值放到 IFD 之前，按 2 字节对齐（TIFF 只要求字对齐）。
  for (auto &entry : entries) {
    if (!entry.out_of_line) {
      continue;
    }
    align_to(out, 2);
    entry.value_offset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), entry.value.begin(), entry.value.end());
  }

  // ---- IFD ----
  align_to(out, 2);
  put_u32(out, ifd_pointer_pos, static_cast<uint32_t>(out.size()));
  std::sort(entries.begin(), entries.end(),
            [](const IfdEntry &a, const IfdEntry &b) { return a.tag < b.tag; });
  append_u16(out, static_cast<uint16_t>(entries.size()));
  for (const auto &entry : entries) {
    append_u16(out, entry.tag);
    append_u16(out, entry.type);
    append_u32(out, entry.count);
    if (entry.out_of_line) {
      append_u32(out, entry.value_offset);
    } else {
      std::vector<uint8_t> inline_value = entry.value;
      inline_value.resize(4, 0);
      out.insert(out.end(), inline_value.begin(), inline_value.end());
    }
  }
  append_u32(out, 0); // 没有下一个 IFD
  return true;
}

bool write_tiff_strips(const char *path, const TiffStripImage &image,
                       std::string &error) {
  std::vector<uint8_t> bytes;
  if (!encode_tiff_strips(image, bytes, error)) {
    return false;
  }
  FILE *file = std::fopen(path, "wb");
  if (file == nullptr) {
    error = std::string("无法写入 ") + (path != nullptr ? path : "(null)");
    return false;
  }
  const bool ok =
      std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  std::fclose(file);
  if (!ok) {
    error = std::string("写入 TIFF 失败 ") + path;
  }
  return ok;
}

bool write_tiff_jpeg_strips(const char *path, const uint8_t *rgb,
                            uint32_t width, uint32_t height,
                            uint32_t rows_per_strip,
                            const JpegStripEncoder &encoder,
                            JpegStripTiffReport &report, std::string &error) {
  report = JpegStripTiffReport{};
  if (rgb == nullptr || width == 0 || height == 0) {
    error = "JPEG strip 写 TIFF：输入为空";
    return false;
  }
  if (!encoder.encode) {
    error = "JPEG strip 写 TIFF：没有提供 strip 编码器";
    return false;
  }
  if (rows_per_strip == 0) {
    error = "rows_per_strip 不能为 0";
    return false;
  }

  std::vector<uint8_t> strip_rgb(
      static_cast<size_t>(width) * rows_per_strip * 3, 0);

  TiffStripImage image;
  image.width = width;
  image.height = height;
  image.rows_per_strip = rows_per_strip;
  image.compression = TiffCompression::kJpeg;
  image.photometric = 6; // YCbCr
  image.strips.reserve((height + rows_per_strip - 1) / rows_per_strip);

  std::vector<uint8_t> reference_tables;
  for (uint32_t y = 0; y < height; y += rows_per_strip) {
    const uint32_t rows_here = std::min(rows_per_strip, height - y);
    // 每条 strip 只编码它真正覆盖的行数（最后一条通常更短）：TIFF 允许
    // 末条 strip 矮于 RowsPerStrip，而一旦把末条补满再编码，libtiff 会拿
    // SOF 高度和它自己算出的 strip 高度比对并报警告。
    for (uint32_t row = 0; row < rows_here; ++row) {
      const uint8_t *src = rgb + static_cast<size_t>(y + row) * width * 3;
      uint8_t *dst = strip_rgb.data() + static_cast<size_t>(row) * width * 3;
      std::memcpy(dst, src, static_cast<size_t>(width) * 3);
    }

    std::vector<uint8_t> jpeg;
    if (!encoder.encode(strip_rgb.data(), width, rows_here, jpeg, error)) {
      return false;
    }

    JpegStreamInfo info;
    if (!parse_jpeg_stream(jpeg.data(), jpeg.size(), info, error)) {
      return false;
    }
    if (info.width != width || info.height != rows_here) {
      error = "编码器返回的 JPEG 尺寸与请求不符（" +
              std::to_string(info.width) + "x" + std::to_string(info.height) +
              " vs " + std::to_string(width) + "x" + std::to_string(rows_here) +
              "）";
      return false;
    }
    if (info.components.size() != 3) {
      error = "TIFF JPEG 只写 3 分量（YCbCr）码流";
      return false;
    }
    if (reference_tables.empty()) {
      reference_tables = info.tables;
      image.subsampling_h = info.components[0].sampling_h;
      image.subsampling_v = info.components[0].sampling_v;
    } else if (info.tables != reference_tables) {
      // 所有 strip 共用一份 JPEGTables，表不同就只能放弃缩写码流。
      // 硬件编码器在同一会话内对同一质量是确定性的，出现差异说明
      // 会话被重建或质量参数变化了，这种情况必须报错而不是写出坏文件。
      error = "各 strip 的量化/霍夫曼表不一致，无法共用 JPEGTables";
      return false;
    }
    std::vector<uint8_t> abbreviated;
    if (!make_abbreviated_jpeg(info, abbreviated, error)) {
      return false;
    }
    image.strips.push_back(std::move(abbreviated));
  }

  image.jpeg_tables = reference_tables;
  report.strips = static_cast<uint32_t>(image.strips.size());
  report.rows_per_strip = rows_per_strip;
  report.subsampling_h = image.subsampling_h;
  report.subsampling_v = image.subsampling_v;
  report.used_jpeg_tables = !image.jpeg_tables.empty();
  report.encoded_width = width;
  return write_tiff_strips(path, image, error);
}

bool write_tiff_rgb_none(const char *path, const uint8_t *rgb, uint32_t width,
                         uint32_t height, uint32_t rows_per_strip,
                         std::string &error) {
  if (rgb == nullptr || width == 0 || height == 0) {
    error = "未压缩 TIFF：输入为空";
    return false;
  }
  if (rows_per_strip == 0) {
    rows_per_strip = height;
  }
  TiffStripImage image;
  image.width = width;
  image.height = height;
  image.rows_per_strip = rows_per_strip;
  image.compression = TiffCompression::kNone;
  image.photometric = 2; // RGB
  const size_t bytes_per_row = static_cast<size_t>(width) * 3;
  for (uint32_t y = 0; y < height; y += rows_per_strip) {
    const uint32_t rows = std::min(rows_per_strip, height - y);
    const uint8_t *src = rgb + static_cast<size_t>(y) * bytes_per_row;
    image.strips.emplace_back(src, src + bytes_per_row * rows);
  }
  return write_tiff_strips(path, image, error);
}

} // namespace tiff
} // namespace oic
