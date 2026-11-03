/*
 * File: tiff_container.h
 * Brief: Tiff container.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/tiff_container.h - TIFF 6.0 容器读写（LE/BE、IFD/Tag 解析与写出）。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace image_codec {
namespace tiff {

// ---- TIFF 6.0 常用 Tag/Type/值常量 ----
enum TiffTag : uint16_t {
  kTagImageWidth = 256,
  kTagImageLength = 257, // ImageHeight
  kTagBitsPerSample = 258,
  kTagCompression = 259,
  kTagPhotometric = 262,
  kTagStripOffsets = 273,
  kTagSamplesPerPixel = 277,
  kTagRowsPerStrip = 278,
  kTagStripByteCounts = 279,
  kTagPlanarConfig = 284,
  kTagTileWidth = 322,
  kTagTileLength = 323,
  kTagTileOffsets = 324,
  kTagTileByteCounts = 325,
  // ExtraSamples 是「spp 里多出来那几个样本是什么」的唯一权威说明。
  // 不写它时，带 Alpha 的 4 通道文件会被解码器当成 CMYK（spp=4 + Photometric=2），
  // 于是同一份字节被解释成完全不同的颜色。
  kTagExtraSamples = 338,
};

enum TiffType : uint16_t {
  kTypeByte = 1,
  kTypeAscii = 2,
  kTypeShort = 3,
  kTypeLong = 4,
  kTypeRational = 5,
};

enum TiffCompressionValue : uint16_t {
  kCompressionNone = 1,
  kCompressionLzw = 5,
};

// ExtraSamples 的取值（TIFF 6.0 §ExtraSamples）：
//   0 = 未指定、1 = 关联 Alpha（预乘）、2 = 非关联 Alpha（直通）。
// 本库写出恒为 2（不预乘），读入时 0/1/2 都当作「有一个 Alpha 样本」，
// 因为「多出来一个样本」这件事本身已经由 spp 决定，语义差别留给上层。
enum TiffExtraSampleValue : uint16_t {
  kExtraUnspecified = 0,
  kExtraAssociatedAlpha = 1,
  kExtraUnassociatedAlpha = 2,
};

// TIFF 字段类型字节数；未知类型返回 0。
inline size_t tiffTypeSize(uint16_t type) {
  switch (type) {
  case kTypeByte:
    return 1;
  case kTypeAscii:
    return 1;
  case kTypeShort:
    return 2;
  case kTypeLong:
    return 4;
  case kTypeRational:
    return 8;
  default:
    return 0;
  }
}

// ---------------------------------------------------------------------------
// TiffReader：解析 header + 第一个 IFD，提供 typed 读取与 strip/tile 数据访问。
// ---------------------------------------------------------------------------
class TiffReader {
public:
  bool openFile(const char *path);
  bool openMemory(const void *data, size_t size);
  void close();

  bool valid() const { return valid_; }
  bool littleEndian() const { return order_ == ByteOrder::Little; }
  uint16_t magic() const { return magic_; }
  uint32_t ifdOffset() const { return ifd_offset_; }
  int entryCount() const { return static_cast<int>(ifd_count_); }
  const char *byteOrderName() const {
    return order_ == ByteOrder::Little ? "little-endian (II)"
                                       : "big-endian (MM)";
  }

  bool hasTag(uint16_t tag) const { return tag_index_.count(tag) != 0; }
  bool getU16(uint16_t tag, uint16_t &v) const;
  bool getU32(uint16_t tag, uint32_t &v) const;
  bool getU32Array(uint16_t tag, std::vector<uint32_t> &v) const;

  // 图像元数据（bps=每样本位数；spp=样本/像素；photometric/compression/planar）。
  bool imageInfo(int &w, int &h, int &spp, int &bps, int &photometric,
                 int &compression, int &planar) const;

  // 是否存在 Alpha 额外样本（ExtraSamples 至少一项）。
  bool hasExtraSamples() const;

  // 布局信息：是 tile 布局还是 strip 布局。
  bool isTiled() const {
    return hasTag(kTagTileWidth) && hasTag(kTagTileOffsets);
  }
  bool getStripInfo(std::vector<uint32_t> &offsets,
                    std::vector<uint32_t> &counts,
                    uint32_t &rows_per_strip) const;
  bool getTileInfo(std::vector<uint32_t> &offsets,
                   std::vector<uint32_t> &counts, uint32_t &tile_w,
                   uint32_t &tile_h) const;

  // 读取 strip/tile 的原始（压缩）字节。
  bool loadStrip(size_t index, std::vector<uint8_t> &out) const;
  bool loadTile(size_t index, std::vector<uint8_t> &out) const;
  bool loadBytes(uint32_t offset, uint32_t length,
                 std::vector<uint8_t> &out) const;

  // 打印文件头信息（供 image_codec_tiff_info）。
  void printInfo(FILE *fp) const;

private:
  enum class ByteOrder { Little, Big };

  struct IfdEntry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    uint8_t value_bytes[4] = {0, 0, 0, 0}; // Value 字段原始 4 字节
    uint32_t value = 0; // value_bytes 按文件字节序解释（数组时为偏移）
  };

  bool parseHeader();
  bool parseIfd();
  bool readEntryRaw(const IfdEntry &e, std::vector<uint8_t> &out) const;
  uint16_t readU16(size_t off) const;
  uint32_t readU32(size_t off) const;
  uint16_t readU16Bytes(const uint8_t *p) const;
  uint32_t readU32Bytes(const uint8_t *p) const;

  std::vector<uint8_t> file_;
  ByteOrder order_ = ByteOrder::Little;
  uint16_t magic_ = 0;
  uint32_t ifd_offset_ = 0;
  uint16_t ifd_count_ = 0;
  std::vector<IfdEntry> entries_;
  std::map<uint16_t, size_t> tag_index_;
  bool valid_ = false;
};

// ---------------------------------------------------------------------------
// TiffWriter：strip 或 tile 布局 TIFF 写出（little-endian）。压缩数据由调用方提供。
// 用法：openStrip(...) -> appendStrip(...) * n -> saveFile(path)。
// ---------------------------------------------------------------------------
class TiffWriter {
public:
  TiffWriter() = default;
  ~TiffWriter() = default;
  TiffWriter(const TiffWriter &) = delete;
  TiffWriter &operator=(const TiffWriter &) = delete;

  void reset();

  // strip 布局。compression: 1=none, 5=LZW。n_strips 个 strip。
  // samples_per_pixel：1 = 灰度、2 = 灰度+Alpha、3 = RGB、4 = RGBA。
  // 这个参数决定 BitsPerSample / SamplesPerPixel / Photometric / ExtraSamples
  // 四个 tag，因此**必须**与调用方实际写出的字节排布一致，不能在外面再猜一次。
  bool openStrip(int w, int h, int rows_per_strip, uint16_t compression,
                 size_t n_strips, int samples_per_pixel = 3);
  // tile 布局（TileWidth/TileLength/TileOffsets/TileByteCounts）。
  bool openTile(int w, int h, uint32_t tile_w, uint32_t tile_h,
                uint16_t compression, size_t n_tiles,
                int samples_per_pixel = 3);

  bool appendStrip(size_t idx, const void *data, size_t size);
  bool appendTile(size_t idx, const void *data, size_t size);

  bool saveFile(const char *path) const;
  // 组装后的完整文件字节。首次访问时才 build()（与 saveFile 同一套惰性逻辑）。
  const std::vector<uint8_t> &buffer() const;
  size_t payloadBytes() const { return data_end_ - data_start_; }

private:
  void build();
  void putU16(size_t at, uint16_t v);
  void putU32(size_t at, uint32_t v);

  bool tiled_ = false;
  int w_ = 0;
  int h_ = 0;
  int rows_per_strip_ = 1;
  uint32_t tile_w_ = 0;
  uint32_t tile_h_ = 0;
  uint16_t compression_ = kCompressionNone;
  int spp_ = 3;
  size_t n_ = 0;
  std::vector<uint32_t> offs_;  // 每 strip/tile 的文件偏移
  std::vector<uint32_t> sizes_; // 每 strip/tile 的字节数
  std::vector<uint8_t> data_;   // payload 区（追加）
  size_t data_start_ = 0;       // payload 区在最终文件中的起始偏移
  size_t data_end_ = 0;         // payload 区结尾（= data_.size()）
  std::vector<uint8_t> buf_;    // 最终组装结果
};

} // namespace tiff
} // namespace image_codec
