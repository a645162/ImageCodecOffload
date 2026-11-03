/*
 * File: tiff_container.cpp
 * Brief: Tiff container.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/tiff_container.cpp - TIFF 6.0 容器读写实现。
#include "tiff_container.h"

#include <cstdio>
#include <cstring>

namespace image_codec {
namespace tiff {

// ---------------------------------------------------------------------------
// TiffReader
// ---------------------------------------------------------------------------
bool TiffReader::openFile(const char *path) {
  close();
  if (path == nullptr) return false;
  FILE *f = std::fopen(path, "rb");
  if (f == nullptr) return false;
  std::fseek(f, 0, SEEK_END);
  long len = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (len <= 0) {
    std::fclose(f);
    return false;
  }
  file_.resize(static_cast<size_t>(len));
  size_t got = std::fread(file_.data(), 1, file_.size(), f);
  std::fclose(f);
  if (got != file_.size()) {
    file_.clear();
    return false;
  }
  return parseHeader() && parseIfd();
}

bool TiffReader::openMemory(const void *data, size_t size) {
  close();
  if (data == nullptr || size < 8) return false;
  const uint8_t *p = static_cast<const uint8_t *>(data);
  file_.assign(p, p + size);
  return parseHeader() && parseIfd();
}

void TiffReader::close() {
  file_.clear();
  entries_.clear();
  tag_index_.clear();
  valid_ = false;
}

bool TiffReader::parseHeader() {
  if (file_.size() < 8) return false;
  if (file_[0] == 'I' && file_[1] == 'I') {
    order_ = ByteOrder::Little;
  } else if (file_[0] == 'M' && file_[1] == 'M') {
    order_ = ByteOrder::Big;
  } else {
    return false;
  }
  magic_ = readU16(2);
  if (magic_ != 42) return false;
  ifd_offset_ = readU32(4);
  if (ifd_offset_ + 2 > file_.size()) return false;
  valid_ = true;
  return true;
}

bool TiffReader::parseIfd() {
  if (!valid_) return false;
  ifd_count_ = readU16(ifd_offset_);
  size_t base = static_cast<size_t>(ifd_offset_) + 2;
  if (base + static_cast<size_t>(ifd_count_) * 12 + 4 > file_.size())
    return false;
  entries_.reserve(ifd_count_);
  for (uint16_t i = 0; i < ifd_count_; ++i) {
    size_t eo = base + static_cast<size_t>(i) * 12;
    IfdEntry e;
    e.tag = readU16(eo);
    e.type = readU16(eo + 2);
    e.count = readU32(eo + 4);
    std::memcpy(e.value_bytes, file_.data() + eo + 8, 4);
    e.value = readU32Bytes(e.value_bytes);
    entries_.push_back(e);
    tag_index_[e.tag] = i;
  }
  return true;
}

uint16_t TiffReader::readU16(size_t off) const {
  if (off + 2 > file_.size()) return 0;
  return readU16Bytes(file_.data() + off);
}

uint32_t TiffReader::readU32(size_t off) const {
  if (off + 4 > file_.size()) return 0;
  return readU32Bytes(file_.data() + off);
}

uint16_t TiffReader::readU16Bytes(const uint8_t *p) const {
  if (order_ == ByteOrder::Little) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
  }
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t TiffReader::readU32Bytes(const uint8_t *p) const {
  if (order_ == ByteOrder::Little) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  }
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool TiffReader::readEntryRaw(const IfdEntry &e,
                              std::vector<uint8_t> &out) const {
  const size_t ts = tiffTypeSize(e.type);
  if (ts == 0) return false;
  const size_t nbytes = ts * e.count;
  out.clear();
  if (nbytes <= 4) {
    // 值内联在 Value 字段前 nbytes 字节（按文件字节序）。
    out.assign(e.value_bytes, e.value_bytes + nbytes);
    return true;
  }
  if (static_cast<uint64_t>(e.value) + nbytes > file_.size()) return false;
  out.assign(file_.begin() + e.value, file_.begin() + e.value + nbytes);
  return true;
}

bool TiffReader::getU16(uint16_t tag, uint16_t &v) const {
  auto it = tag_index_.find(tag);
  if (it == tag_index_.end()) return false;
  const IfdEntry &e = entries_[it->second];
  if (e.count == 0) return false;
  if (e.type == kTypeShort) {
    std::vector<uint8_t> raw;
    if (!readEntryRaw(e, raw)) return false;
    v = readU16Bytes(raw.data());
    return true;
  }
  if (e.type == kTypeLong) {
    std::vector<uint8_t> raw;
    if (!readEntryRaw(e, raw)) return false;
    uint32_t u = readU32Bytes(raw.data());
    v = static_cast<uint16_t>(u);
    return true;
  }
  if (e.type == kTypeByte) {
    std::vector<uint8_t> raw;
    if (!readEntryRaw(e, raw)) return false;
    v = raw[0];
    return true;
  }
  return false;
}

bool TiffReader::getU32(uint16_t tag, uint32_t &v) const {
  auto it = tag_index_.find(tag);
  if (it == tag_index_.end()) return false;
  const IfdEntry &e = entries_[it->second];
  if (e.count == 0) return false;
  if (e.type == kTypeLong) {
    std::vector<uint8_t> raw;
    if (!readEntryRaw(e, raw)) return false;
    v = readU32Bytes(raw.data());
    return true;
  }
  if (e.type == kTypeShort) {
    uint16_t s = 0;
    if (!getU16(tag, s)) return false;
    v = s;
    return true;
  }
  return false;
}

bool TiffReader::getU32Array(uint16_t tag, std::vector<uint32_t> &v) const {
  auto it = tag_index_.find(tag);
  if (it == tag_index_.end()) return false;
  const IfdEntry &e = entries_[it->second];
  v.clear();
  if (e.count == 0) return true;
  if (e.type == kTypeShort) {
    std::vector<uint8_t> raw;
    if (!readEntryRaw(e, raw)) return false;
    v.reserve(e.count);
    for (uint32_t i = 0; i < e.count; ++i) {
      v.push_back(readU16Bytes(raw.data() + i * 2));
    }
    return true;
  }
  if (e.type == kTypeLong) {
    std::vector<uint8_t> raw;
    if (!readEntryRaw(e, raw)) return false;
    v.reserve(e.count);
    for (uint32_t i = 0; i < e.count; ++i) {
      v.push_back(readU32Bytes(raw.data() + i * 4));
    }
    return true;
  }
  return false;
}

bool TiffReader::imageInfo(int &w, int &h, int &spp, int &bps, int &photometric,
                           int &compression, int &planar) const {
  uint16_t u = 0;
  if (!getU16(kTagImageWidth, u)) return false;
  w = u;
  if (!getU16(kTagImageLength, u)) return false;
  h = u;
  // 可选 tag 按 TIFF 默认值补齐（缺失时）。
  if (!getU16(kTagSamplesPerPixel, u)) spp = 1;
  else spp = u;
  if (!getU16(kTagBitsPerSample, u)) bps = 1;
  else bps = u;
  if (!getU16(kTagPhotometric, u)) return false;
  photometric = u;
  if (!getU16(kTagCompression, u)) compression = kCompressionNone;
  else compression = u;
  if (!getU16(kTagPlanarConfig, u)) planar = 1;
  else planar = u;
  return true;
}

bool TiffReader::hasExtraSamples() const {
  uint16_t v = 0;
  return getU16(kTagExtraSamples, v);
}

bool TiffReader::getStripInfo(std::vector<uint32_t> &offsets,
                              std::vector<uint32_t> &counts,
                              uint32_t &rows_per_strip) const {
  if (!getU32Array(kTagStripOffsets, offsets)) return false;
  if (!getU32Array(kTagStripByteCounts, counts)) return false;
  if (offsets.size() != counts.size() || offsets.empty()) return false;
  if (!getU32(kTagRowsPerStrip, rows_per_strip)) rows_per_strip = 0;
  return true;
}

bool TiffReader::getTileInfo(std::vector<uint32_t> &offsets,
                             std::vector<uint32_t> &counts, uint32_t &tile_w,
                             uint32_t &tile_h) const {
  if (!getU32Array(kTagTileOffsets, offsets)) return false;
  if (!getU32Array(kTagTileByteCounts, counts)) return false;
  if (offsets.size() != counts.size() || offsets.empty()) return false;
  if (!getU32(kTagTileWidth, tile_w)) return false;
  if (!getU32(kTagTileLength, tile_h)) return false;
  return true;
}

bool TiffReader::loadBytes(uint32_t offset, uint32_t length,
                           std::vector<uint8_t> &out) const {
  out.clear();
  if (static_cast<uint64_t>(offset) + length > file_.size()) return false;
  out.assign(file_.begin() + offset, file_.begin() + offset + length);
  return true;
}

bool TiffReader::loadStrip(size_t index, std::vector<uint8_t> &out) const {
  std::vector<uint32_t> offs, counts;
  uint32_t rps = 0;
  if (!getStripInfo(offs, counts, rps)) return false;
  if (index >= offs.size()) return false;
  return loadBytes(offs[index], counts[index], out);
}

bool TiffReader::loadTile(size_t index, std::vector<uint8_t> &out) const {
  std::vector<uint32_t> offs, counts;
  uint32_t tw = 0, th = 0;
  if (!getTileInfo(offs, counts, tw, th)) return false;
  if (index >= offs.size()) return false;
  return loadBytes(offs[index], counts[index], out);
}

void TiffReader::printInfo(FILE *fp) const {
  int w = 0, h = 0, spp = 1, bps = 8, photometric = 0, compression = 1,
      planar = 1;
  std::fprintf(fp, "  Byte order      : %s\n", byteOrderName());
  std::fprintf(fp, "  Magic           : %u\n", static_cast<unsigned>(magic_));
  std::fprintf(fp, "  IFD offset      : %u (%d entries)\n", ifd_offset_,
               static_cast<int>(ifd_count_));
  if (!imageInfo(w, h, spp, bps, photometric, compression, planar)) {
    std::fprintf(fp, "  (core tags missing or unsupported)\n");
    return;
  }
  std::fprintf(fp, "  Image size      : %d x %d\n", w, h);
  std::fprintf(fp, "  Bits/sample     : %d (x%d)\n", bps, spp);
  std::fprintf(fp, "  Samples/pixel   : %d\n", spp);
  std::fprintf(fp, "  Photometric     : %d%s\n", photometric,
               photometric == 2 ? " (RGB)" : "");
  std::fprintf(fp, "  Compression     : %d%s\n", compression,
               compression == 1   ? " (none)"
               : compression == 5 ? " (LZW)"
                                  : "");
  std::fprintf(fp, "  Planar config   : %d%s\n", planar,
               planar == 1 ? " (chunky)" : " (planar)");
  if (isTiled()) {
    std::vector<uint32_t> offs, counts;
    uint32_t tw = 0, th = 0;
    if (getTileInfo(offs, counts, tw, th)) {
      std::fprintf(fp, "  Tile            : %ux%u, %zu tiles\n", tw, th,
                   offs.size());
    }
  } else {
    std::vector<uint32_t> offs, counts;
    uint32_t rps = 0;
    if (getStripInfo(offs, counts, rps)) {
      std::fprintf(fp, "  RowsPerStrip    : %u\n", rps);
      std::fprintf(fp, "  Strips          : %zu\n", offs.size());
    }
  }
}

// ---------------------------------------------------------------------------
// TiffWriter
// ---------------------------------------------------------------------------
void TiffWriter::reset() {
  tiled_ = false;
  w_ = h_ = 0;
  rows_per_strip_ = 1;
  tile_w_ = tile_h_ = 0;
  compression_ = kCompressionNone;
  spp_ = 3;
  n_ = 0;
  offs_.clear();
  sizes_.clear();
  data_.clear();
  data_start_ = data_end_ = 0;
  buf_.clear();
}

bool TiffWriter::openStrip(int w, int h, int rows_per_strip,
                           uint16_t compression, size_t n_strips,
                           int samples_per_pixel) {
  reset();
  if (w <= 0 || h <= 0 || rows_per_strip <= 0 || n_strips == 0) return false;
  // 1 = 灰度、2 = 灰度+A、3 = RGB、4 = RGBA（后两者的第 4 个样本写 ExtraSamples）。
  if (samples_per_pixel < 1 || samples_per_pixel > 4) return false;
  tiled_ = false;
  w_ = w;
  h_ = h;
  rows_per_strip_ = rows_per_strip;
  compression_ = compression;
  spp_ = samples_per_pixel;
  n_ = n_strips;
  offs_.assign(n_, 0);
  sizes_.assign(n_, 0);
  return true;
}

bool TiffWriter::openTile(int w, int h, uint32_t tile_w, uint32_t tile_h,
                          uint16_t compression, size_t n_tiles,
                          int samples_per_pixel) {
  reset();
  if (w <= 0 || h <= 0 || tile_w == 0 || tile_h == 0 || n_tiles == 0)
    return false;
  if (samples_per_pixel < 1 || samples_per_pixel > 4) return false;
  tiled_ = true;
  w_ = w;
  h_ = h;
  tile_w_ = tile_w;
  tile_h_ = tile_h;
  compression_ = compression;
  spp_ = samples_per_pixel;
  n_ = n_tiles;
  offs_.assign(n_, 0);
  sizes_.assign(n_, 0);
  return true;
}

bool TiffWriter::appendStrip(size_t idx, const void *data, size_t size) {
  if (tiled_ || idx >= n_ || data == nullptr) return false;
  offs_[idx] = static_cast<uint32_t>(data_.size());
  sizes_[idx] = static_cast<uint32_t>(size);
  const uint8_t *p = static_cast<const uint8_t *>(data);
  data_.insert(data_.end(), p, p + size);
  return true;
}

bool TiffWriter::appendTile(size_t idx, const void *data, size_t size) {
  if (!tiled_ || idx >= n_ || data == nullptr) return false;
  offs_[idx] = static_cast<uint32_t>(data_.size());
  sizes_[idx] = static_cast<uint32_t>(size);
  const uint8_t *p = static_cast<const uint8_t *>(data);
  data_.insert(data_.end(), p, p + size);
  return true;
}

void TiffWriter::putU16(size_t at, uint16_t v) {
  buf_[at] = static_cast<uint8_t>(v & 0xFF);
  buf_[at + 1] = static_cast<uint8_t>(v >> 8);
}

void TiffWriter::putU32(size_t at, uint32_t v) {
  buf_[at] = static_cast<uint8_t>(v & 0xFF);
  buf_[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  buf_[at + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  buf_[at + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void TiffWriter::build() {
  // ExtraSamples 只在「多出来的样本是 Alpha」时出现：spp=2（灰度+A）与
  // spp=4（RGB+A）。spp=3 是纯 RGB，没有多出来的样本；spp>4 本库不支持。
  const bool has_extra = (spp_ == 2 || spp_ == 4);
  const size_t n_tags = (tiled_ ? 11 : 10) + (has_extra ? 1 : 0);
  const size_t ifd_size = 2 + n_tags * 12 + 4;

  // BitsPerSample 需要数组区的条件是「数组总字节数 > 4」，即 spp >= 3。
  // spp==1（1 个 SHORT）与 spp==2（2 个 SHORT，恰好 4 字节）都必须内联进
  // Value 字段。曾经按 spp>1 统一走数组，于是 spp==2 时 Value 存的是数组偏移
  // 146，而按 TIFF 规范读回来会被当成「位深 146」，解码器直接拒绝。
  const bool bps_array = spp_ >= 3;
  size_t off = 8 + ifd_size;
  size_t off_bps = 0;
  if (bps_array) {
    off_bps = off;
    off += static_cast<size_t>(spp_) * 2;
  }
  const uint32_t bps_value =
      spp_ == 1 ? 8u
                : (spp_ == 2 ? 0x00080008u : static_cast<uint32_t>(off_bps));
  // LONG 型偏移/计数数组按 4 字节对齐（灰度少一段 BitsPerSample 数组时会错位）。
  off = (off + 3) & ~static_cast<size_t>(3);
  const size_t off_off = off;
  off += n_ * 4;
  const size_t off_cnt = off;
  off += n_ * 4;
  data_start_ = off;
  data_end_ = data_start_ + data_.size();

  buf_.assign(data_end_, 0);
  buf_[0] = 'I';
  buf_[1] = 'I';
  buf_[2] = 42;
  buf_[3] = 0;
  putU32(4, 8); // IFD offset

  size_t e = 8;
  putU16(e, static_cast<uint16_t>(n_tags));
  e += 2;

  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t val) {
    putU16(e, tag);
    putU16(e + 2, type);
    putU32(e + 4, count);
    putU32(e + 8, val);
    e += 12;
  };

  // 根因修复（单 strip/单 tile 容器错误）：
  // 对 count==1 的 LONG 型 tag，TIFF 规范要求把实际值直接写在 Value 字段；
  // 只有 count>1 时 Value 字段才存"数组的偏移地址"（off_off/off_cnt）。
  // 旧实现一律写 off_off/off_cnt（数组地址），导致单 strip（n_==1）文件里
  // StripOffsets 被读成 140（偏移数组自身地址）、StripByteCounts 被读成 144，
  // 而真实条带数据在 148 处、长度 46076——解码器读到"数组字节 + 截断的码流"
  // 必然解码失败（image_codec 解码 -11 / ffmpeg "Decoded only 0 bytes"）。
  // 多 strip（n_>1）时地址语义正确，故仅 count==1 需写入实际值。
  // 该缺陷与 LZW 字典满无关：小图（不触发字典满）单 strip 同样损坏，
  // 而修复后完整码流（含多次字典满 ClearCode）可逐字节正确解码。
  if (!tiled_) {
    entry(kTagImageWidth, kTypeLong, 1, static_cast<uint32_t>(w_));
    entry(kTagImageLength, kTypeLong, 1, static_cast<uint32_t>(h_));
    entry(kTagBitsPerSample, kTypeShort, static_cast<uint32_t>(spp_),
          bps_value);
    entry(kTagCompression, kTypeShort, 1, compression_);
    // Photometric：1 = BlackIsZero（灰度），2 = RGB。写错这个 tag 会让解码器
    // 把单通道数据当成红色通道；而 spp==4 少写 ExtraSamples 会被当成 CMYK。
    entry(kTagPhotometric, kTypeShort, 1, spp_ <= 2 ? 1u : 2u);
    entry(kTagStripOffsets, kTypeLong, static_cast<uint32_t>(n_),
          static_cast<uint32_t>(n_ == 1 ? data_start_ + offs_[0] : off_off));
    entry(kTagSamplesPerPixel, kTypeShort, 1, static_cast<uint32_t>(spp_));
    entry(kTagRowsPerStrip, kTypeLong, 1,
          static_cast<uint32_t>(rows_per_strip_));
    entry(kTagStripByteCounts, kTypeLong, static_cast<uint32_t>(n_),
          static_cast<uint32_t>(n_ == 1 ? sizes_[0] : off_cnt));
    entry(kTagPlanarConfig, kTypeShort, 1, 1); // chunky
  } else {
    entry(kTagImageWidth, kTypeLong, 1, static_cast<uint32_t>(w_));
    entry(kTagImageLength, kTypeLong, 1, static_cast<uint32_t>(h_));
    entry(kTagBitsPerSample, kTypeShort, static_cast<uint32_t>(spp_),
          bps_value);
    entry(kTagCompression, kTypeShort, 1, compression_);
    entry(kTagPhotometric, kTypeShort, 1, spp_ == 1 ? 1u : 2u);
    entry(kTagSamplesPerPixel, kTypeShort, 1, static_cast<uint32_t>(spp_));
    entry(kTagPlanarConfig, kTypeShort, 1, 1);
    entry(kTagTileWidth, kTypeLong, 1, tile_w_);
    entry(kTagTileLength, kTypeLong, 1, tile_h_);
    entry(kTagTileOffsets, kTypeLong, static_cast<uint32_t>(n_),
          static_cast<uint32_t>(n_ == 1 ? data_start_ + offs_[0] : off_off));
    entry(kTagTileByteCounts, kTypeLong, static_cast<uint32_t>(n_),
          static_cast<uint32_t>(n_ == 1 ? sizes_[0] : off_cnt));
  }

  // ExtraSamples 的 tag 号（338）大于上面所有 tag，因此统一追加在最后，
  // 保证 IFD 里的 tag 严格升序（部分解码器依赖这一点做二分查找）。
  // count == 1：本库最多只有一个额外样本（Alpha）；值 2 = 非关联（不预乘）。
  if (has_extra) {
    entry(kTagExtraSamples, kTypeShort, 1, kExtraUnassociatedAlpha);
  }

  putU32(e, 0); // next IFD

  // BitsPerSample {8,...}（仅 spp>=3 时需要数组区）
  if (bps_array) {
    for (int k = 0; k < spp_; ++k) {
      putU16(off_bps + static_cast<size_t>(k) * 2, 8);
    }
  }
  // 偏移数组 + 计数数组（相对 data_ 的偏移 + data_start_）
  for (size_t i = 0; i < n_; ++i) {
    putU32(off_off + i * 4, static_cast<uint32_t>(data_start_ + offs_[i]));
  }
  for (size_t i = 0; i < n_; ++i) {
    putU32(off_cnt + i * 4, sizes_[i]);
  }
  // payload
  if (!data_.empty()) {
    std::memcpy(buf_.data() + data_start_, data_.data(), data_.size());
  }
}

const std::vector<uint8_t> &TiffWriter::buffer() const {
  // build() 是写 buf_ 的惰性步骤；调用方拿到 buffer() 时文件头还没算出来，
  // 直接返回空 vector 会让「编码成功但产物是空」这种最坏的 bug 悄悄通过。
  TiffWriter *self = const_cast<TiffWriter *>(this);
  if (self->buf_.empty() && !self->offs_.empty()) self->build();
  return buf_;
}

bool TiffWriter::saveFile(const char *path) const {
  if (path == nullptr || offs_.empty()) return false;
  // 非 const 复制构建（build 写 buf_）。
  TiffWriter *self = const_cast<TiffWriter *>(this);
  if (self->buf_.empty()) self->build();
  FILE *f = std::fopen(path, "wb");
  if (f == nullptr) return false;
  bool ok = std::fwrite(self->buf_.data(), 1, self->buf_.size(), f) ==
            self->buf_.size();
  std::fclose(f);
  return ok;
}

} // namespace tiff
} // namespace image_codec
