/*
 * File: tiff_container.cpp
 * Brief: classic TIFF 容器的拼装（复刻 libtiff 4.7.2 的目录布局）。
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-11-01
 */

#include "tiff_container.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace image_codec {
namespace offload {
namespace cpu {

namespace {

// TIFF 6.0 的标签值。这里刻意不 include <tiffio.h>：容器拼装是纯字节操作，
// 与 libtiff 的编码器无关，保持独立才能被单测直接驱动。
constexpr unsigned kTagImageWidth = 256;
constexpr unsigned kTagImageLength = 257;
constexpr unsigned kTagBitsPerSample = 258;
constexpr unsigned kTagCompression = 259;
constexpr unsigned kTagPhotometric = 262;
constexpr unsigned kTagStripOffsets = 273;
constexpr unsigned kTagOrientation = 274;
constexpr unsigned kTagSamplesPerPixel = 277;
constexpr unsigned kTagRowsPerStrip = 278;
constexpr unsigned kTagStripByteCounts = 279;
constexpr unsigned kTagXResolution = 282;
constexpr unsigned kTagYResolution = 283;
constexpr unsigned kTagPlanarConfig = 284;
constexpr unsigned kTagResolutionUnit = 296;
constexpr unsigned kTagPredictor = 317;
constexpr unsigned kTagExtraSamples = 338;

constexpr unsigned kTypeShort = 3;
constexpr unsigned kTypeLong = 4;
constexpr unsigned kTypeRational = 5;

constexpr unsigned kPhotometricMinIsBlack = 1;
constexpr unsigned kPhotometricRgb = 2;
constexpr unsigned kPlanarConfigContig = 1;
constexpr unsigned kOrientationTopLeft = 1;
constexpr unsigned kResUnitInch = 2;
constexpr unsigned kResUnitCentimeter = 3;
constexpr unsigned kExtraSampleUnassAlpha = 2;

constexpr unsigned kCompressionNone = 1;
constexpr unsigned kCompressionLzw = 5;
constexpr unsigned kCompressionJpeg = 7;
constexpr unsigned kCompressionAdobeDeflate = 8;
constexpr unsigned kCompressionDeflate = 32946;
constexpr unsigned kCompressionPackBits = 32773;

// 一条待写进目录的条目。data 是已按类型序列化好的小端字节；长度 <= 4 时
// 直接内联在条目里，否则要落到目录之后的「额外值区」。
struct Entry final {
  unsigned tag = 0;
  unsigned type = 0;
  unsigned count = 0;
  std::vector<std::uint8_t> data;
  std::size_t extra_offset = 0; // 0 = 内联（额外值区从不会落在偏移 0）
};

void put16(std::vector<std::uint8_t> *out, std::size_t at, unsigned value) {
  (*out)[at] = static_cast<std::uint8_t>(value & 0xFFU);
  (*out)[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

void put32(std::vector<std::uint8_t> *out, std::size_t at, unsigned value) {
  for (int i = 0; i < 4; ++i) {
    (*out)[at + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
  }
}

void append16(std::vector<std::uint8_t> *out, unsigned value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void append32(std::vector<std::uint8_t> *out, unsigned value) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
  }
}

// libtiff 的 `_WriteAsType(tif, TIFFStripSize64(tif), 0xFFFF)`。
//
// 这个判定**依赖数据**：同一个写文件函数，片大一点就得从 SHORT 换成 LONG，
// 目录因此多出 2 字节/条。不在这里复刻，拼出来的 IFD 就会与整图编码差几个
// 字节，而文件本身完全合法——这种差异最难被发现。
bool writeByteCountsAsLong(const int compression,
                           const std::uint64_t strip_size,
                           const unsigned count) noexcept {
  // count == 1 时 libtiff 不做选择，一律 LONG。
  if (count <= 1) return true;
  switch (compression) {
  case kCompressionNone:
    return strip_size > 0xFFFFU;
  case kCompressionJpeg:
  case kCompressionLzw:
  case kCompressionAdobeDeflate:
  case kCompressionDeflate:
    // 「最坏情况压缩后是原始的 10 倍」，所以阈值也除以 10。
    return strip_size >= (0xFFFFU / 10U);
  default:
    // LZMA / LERC / ZSTD / WEBP / JXL / 以及其它（含 PackBits）都按 LONG 写。
    return true;
  }
}

Entry makeShort(const unsigned tag, const unsigned value) {
  Entry entry;
  entry.tag = tag;
  entry.type = kTypeShort;
  entry.count = 1;
  append16(&entry.data, value);
  return entry;
}

// libtiff 的 TIFFWriteDirectoryTagShortLong：值 <= 0xFFFF 用 SHORT，
// 否则用 LONG（ImageWidth / ImageLength / RowsPerStrip 都走这条路）。
Entry makeShortLong(const unsigned tag, const std::uint32_t value) {
  Entry entry;
  entry.tag = tag;
  if (value <= 0xFFFFU) {
    entry.type = kTypeShort;
    entry.count = 1;
    append16(&entry.data, value);
  } else {
    entry.type = kTypeLong;
    entry.count = 1;
    append32(&entry.data, value);
  }
  return entry;
}

Entry makeShortArray(const unsigned tag, const std::vector<unsigned> &values) {
  Entry entry;
  entry.tag = tag;
  entry.type = kTypeShort;
  entry.count = static_cast<unsigned>(values.size());
  for (const unsigned value : values) {
    append16(&entry.data, value);
  }
  return entry;
}

Entry makeLongArray(const unsigned tag,
                    const std::vector<std::uint32_t> &values) {
  Entry entry;
  entry.tag = tag;
  entry.type = kTypeLong;
  entry.count = static_cast<unsigned>(values.size());
  for (const std::uint32_t value : values) {
    append32(&entry.data, value);
  }
  return entry;
}

// DPI 只支持整数（C API 传进来的就是 int）。libtiff 对整数值走
// DoubleToRational 的「整数直通」分支，得到 num=value / denom=1，与此一致。
Entry makeRational(const unsigned tag, const unsigned numerator) {
  Entry entry;
  entry.tag = tag;
  entry.type = kTypeRational;
  entry.count = 1;
  append32(&entry.data, numerator);
  append32(&entry.data, 1);
  return entry;
}

}  // namespace

bool compressionSupportsPredictor(const int compression) noexcept {
  switch (compression) {
  case kCompressionLzw:
  case kCompressionAdobeDeflate:
  case kCompressionDeflate:
    return true;
  default:
    // NONE / PackBits 没有预测器编解码器；libtiff 会直接忽略该标签（见头文件）。
    return false;
  }
}

bool compressionSupportsStripSharding(const int compression) noexcept {
  switch (compression) {
  case kCompressionNone:
  case kCompressionLzw:
  case kCompressionAdobeDeflate:
  case kCompressionDeflate:
  case kCompressionPackBits:
    return true;
  default:
    // compression = 7（JPEG-in-TIFF）依赖 TIFFTAG_JPEGTABLES 共享量化表，
    // 单独压一条 strip 会得到一张缺表的图；tiled 布局的字符串则是 Tile*
    // 数组而不是 Strip* 数组，拼装规则完全不同。两者都不在分片范围内。
    return false;
  }
}

Result<std::vector<std::uint8_t>>
assembleClassicTiff(const TiffLayout& layout,
                    const std::vector<std::vector<std::uint8_t>> &strips) {
  if (layout.width <= 0 || layout.height <= 0) {
    return fail<std::vector<std::uint8_t>>(
        Status::InvalidArgument, "tiff container: invalid dimensions");
  }
  if (layout.channels < 1 || layout.channels > 4) {
    return fail<std::vector<std::uint8_t>>(
        Status::UnsupportedFormat, "tiff container: unsupported channel count");
  }
  if (layout.rows_per_strip <= 0) {
    return fail<std::vector<std::uint8_t>>(
        Status::InvalidArgument, "tiff container: rows_per_strip must be > 0");
  }
  if (!compressionSupportsStripSharding(layout.compression)) {
    return fail<std::vector<std::uint8_t>>(
        Status::UnsupportedFormat,
        "tiff container: compression does not support strip assembly");
  }

  const std::size_t expected_strips =
      (static_cast<std::size_t>(layout.height) +
       static_cast<std::size_t>(layout.rows_per_strip) - 1U) /
      static_cast<std::size_t>(layout.rows_per_strip);
  if (strips.size() != expected_strips) {
    return fail<std::vector<std::uint8_t>>(
        Status::InvalidArgument,
        "tiff container: strip count does not match rows_per_strip");
  }

  std::vector<std::uint8_t> out;
  out.reserve(8);
  // 头：II + 42 + 目录偏移（目录偏移稍后回填）。
  out.push_back('I');
  out.push_back('I');
  append16(&out, 42);
  append32(&out, 0);

  std::vector<std::uint32_t> offsets;
  std::vector<std::uint32_t> counts;
  offsets.reserve(strips.size());
  counts.reserve(strips.size());
  for (const auto &strip : strips) {
    if (out.size() > 0xFFFFFFFFULL) {
      return fail<std::vector<std::uint8_t>>(
          Status::OutOfMemory,
          "tiff container: offset exceeds classic TIFF 4 GiB limit");
    }
    offsets.push_back(static_cast<std::uint32_t>(out.size()));
    counts.push_back(static_cast<std::uint32_t>(strip.size()));
    out.insert(out.end(), strip.begin(), strip.end());
  }

  // 目录起点偶数对齐（libtiff 的 TIFFWriteDirectorySec 同样如此）。
  if ((out.size() & 1U) != 0U) out.push_back(0);

  const std::size_t ifd_at = out.size();

  std::vector<Entry> entries;
  entries.reserve(16);
  entries.push_back(
      makeShortLong(kTagImageWidth, static_cast<std::uint32_t>(layout.width)));
  entries.push_back(makeShortLong(kTagImageLength,
                                  static_cast<std::uint32_t>(layout.height)));

  // 分辨率只在实际给了 DPI 时写（与整图编码同一条件）。
  if (layout.x_dpi > 0 || layout.y_dpi > 0) {
    const int x_dpi = layout.x_dpi > 0 ? layout.x_dpi : 72;
    const int y_dpi = layout.y_dpi > 0 ? layout.y_dpi : x_dpi;
    entries.push_back(
        makeRational(kTagXResolution, static_cast<unsigned>(x_dpi)));
    entries.push_back(
        makeRational(kTagYResolution, static_cast<unsigned>(y_dpi)));
  }

  entries.push_back(makeShortArray(
      kTagBitsPerSample,
      std::vector<unsigned>(static_cast<std::size_t>(layout.channels), 8)));
  entries.push_back(
      makeShort(kTagCompression, static_cast<unsigned>(layout.compression)));
  entries.push_back(makeShort(kTagPhotometric, layout.channels <= 2
                                                   ? kPhotometricMinIsBlack
                                                   : kPhotometricRgb));
  entries.push_back(makeShort(kTagOrientation, kOrientationTopLeft));
  entries.push_back(
      makeShort(kTagSamplesPerPixel, static_cast<unsigned>(layout.channels)));
  entries.push_back(makeShortLong(
      kTagRowsPerStrip, static_cast<std::uint32_t>(layout.rows_per_strip)));
  entries.push_back(makeShort(kTagPlanarConfig, kPlanarConfigContig));
  if (layout.x_dpi > 0 || layout.y_dpi > 0) {
    entries.push_back(makeShort(kTagResolutionUnit, layout.resolution_unit == 3
                                                        ? kResUnitCentimeter
                                                        : kResUnitInch));
  }
  if (layout.predictor > 0) {
    entries.push_back(
        makeShort(kTagPredictor, static_cast<unsigned>(layout.predictor)));
  }

  const std::uint64_t nominal_rows =
      static_cast<std::uint64_t>(layout.rows_per_strip) <
              static_cast<std::uint64_t>(layout.height)
          ? static_cast<std::uint64_t>(layout.rows_per_strip)
          : static_cast<std::uint64_t>(layout.height);
  const std::uint64_t strip_size = nominal_rows *
                                   static_cast<std::uint64_t>(layout.width) *
                                   static_cast<std::uint64_t>(layout.channels);
  if (writeByteCountsAsLong(layout.compression, strip_size,
                            static_cast<unsigned>(counts.size()))) {
    entries.push_back(makeLongArray(kTagStripByteCounts, counts));
  } else {
    Entry entry;
    entry.tag = kTagStripByteCounts;
    entry.type = kTypeShort;
    entry.count = static_cast<unsigned>(counts.size());
    for (const std::uint32_t value : counts) {
      append16(&entry.data, value);
    }
    entries.push_back(std::move(entry));
  }
  entries.push_back(makeLongArray(kTagStripOffsets, offsets));

  if (layout.channels == 2 || layout.channels == 4) {
    entries.push_back(
        makeShortArray(kTagExtraSamples, {kExtraSampleUnassAlpha}));
  }

  // 目录内条目按 tag 升序（libtiff 会做同样的事）。
  std::sort(entries.begin(), entries.end(),
            [](const Entry &a, const Entry &b) { return a.tag < b.tag; });

  const std::size_t entries_at = ifd_at + 2;
  const std::size_t extra_at = entries_at + 12U * entries.size() + 4U;
  out.resize(extra_at, 0);
  put32(&out, 4, static_cast<unsigned>(ifd_at));
  put16(&out, ifd_at, static_cast<unsigned>(entries.size()));

  // 额外值区的顺序**不是**按 tag 升序，而是 libtiff 在
  // TIFFWriteDirectorySec() 里调用 TIFFWriteDirectoryTagXxx 的顺序：
  //   ImageDimensions → Resolution(282,283) → SubfileType → BitsPerSample(258)
  //   → Compression..PlanarConfig → ResolutionUnit → StripByteCounts(279)
  //   → StripOffsets(273) → ColorMap → ExtraSamples(338) → SampleFormat(339)
  // 每个值在调用时立刻追加到目录之后，所以文件尾部就是上面这个顺序。
  static constexpr unsigned kExtraOrder[] = {282, 283, 258, 279, 273, 338, 339};

  std::size_t cursor = extra_at;
  const auto find_entry = [&entries](const unsigned tag) -> Entry * {
    for (auto &entry : entries) {
      if (entry.tag == tag) return &entry;
    }
    return nullptr;
  };
  for (const unsigned tag : kExtraOrder) {
    Entry *entry = find_entry(tag);
    if (entry == nullptr || entry->data.size() <= 4U) continue;
    entry->extra_offset = cursor;
    out.insert(out.end(), entry->data.begin(), entry->data.end());
    cursor += entry->data.size();
  }
  // 兜底：本函数目前不会产生别的多值标签，但漏掉一个会让它的偏移保持 0
  // （写到文件头里），因此这里显式排到后面而不是静默丢弃。
  for (auto &entry : entries) {
    if (entry.data.size() <= 4U || entry.extra_offset != 0) continue;
    entry.extra_offset = cursor;
    out.insert(out.end(), entry.data.begin(), entry.data.end());
    cursor += entry.data.size();
  }

  for (std::size_t index = 0; index < entries.size(); ++index) {
    const Entry &entry = entries[index];
    const std::size_t at = entries_at + index * 12U;
    put16(&out, at, entry.tag);
    put16(&out, at + 2, entry.type);
    put32(&out, at + 4, entry.count);
    if (entry.data.size() <= 4U) {
      for (std::size_t byte = 0; byte < entry.data.size(); ++byte) {
        out[at + 8 + byte] = entry.data[byte];
      }
      for (std::size_t byte = entry.data.size(); byte < 4U; ++byte) {
        out[at + 8 + byte] = 0;
      }
    } else {
      put32(&out, at + 8, static_cast<unsigned>(entry.extra_offset));
    }
  }
  // 最后一个目录的「下一个目录」指针为 0。
  put32(&out, entries_at + 12U * entries.size(), 0);

  if (out.size() > 0xFFFFFFFFULL) {
    return fail<std::vector<std::uint8_t>>(
        Status::OutOfMemory,
        "tiff container: file exceeds classic TIFF 4 GiB limit");
  }
  return out;
}

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
