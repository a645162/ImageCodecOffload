/*
 * File: jpeg_huffman.cpp
 * Brief: Jpeg huffman.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/jpeg_huffman.cpp - CPU 熵编解码实现。
#include "jpeg_huffman.h"

#include <cmath>
#include <cstring>

namespace image_codec {
namespace jpeg {

// ---- 位读取器 ----

int JpegBitReader::get_bits(int n) {
  if (restart_marker_ >= 0 || eof_) return -1;
  while (bitcnt_ < n) {
    if (pos_ >= size_) {
      eof_ = true;
      return -1;
    }
    uint8_t b = data_[pos_++];
    if (b == 0xFF) {
      if (pos_ >= size_) {
        eof_ = true;
        return -1;
      }
      const uint8_t b2 = data_[pos_++];
      if (b2 == 0x00) {
        b = 0xFF; // 0xFF00 填充字节 -> 0xFF
      } else if (b2 >= 0xD0 && b2 <= 0xD7) {
        restart_marker_ = b2 - 0xD0;
        return -1;
      } else {
        eof_ = true; // EOI 或其它标记：扫描结束
        return -1;
      }
    }
    bitbuf_ = (bitbuf_ << 8) | b;
    bitcnt_ += 8;
  }
  bitcnt_ -= n;
  const uint32_t mask = (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1);
  const int v = static_cast<int>((bitbuf_ >> bitcnt_) & mask);
  bitbuf_ &= (bitcnt_ > 0) ? ((1u << bitcnt_) - 1) : 0;
  return v;
}

bool JpegBitReader::consume_restart() {
  if (restart_marker_ >= 0) {
    // 标记已在 get_bits 中被消费，只需复位缓冲。
    bitbuf_ = 0;
    bitcnt_ = 0;
    restart_marker_ = -1;
    return true;
  }
  // 丢弃填充位（RST 前熵数据按字节对齐，残余位为 1 填充）。
  bitbuf_ = 0;
  bitcnt_ = 0;
  while (pos_ < size_) {
    if (data_[pos_++] == 0xFF) {
      if (pos_ >= size_) {
        eof_ = true;
        return false;
      }
      const uint8_t b2 = data_[pos_++];
      if (b2 == 0x00) continue; // 填充字节，继续找
      if (b2 >= 0xD0 && b2 <= 0xD7) {
        restart_marker_ = -1;
        return true;
      }
      eof_ = true;
      restart_marker_ = -1;
      return false; // 遇到 EOI 等：缺失 RST
    }
  }
  eof_ = true;
  restart_marker_ = -1;
  return false;
}

// ---- 解码 ----

namespace {

// 解码一个 Huffman 符号。返回符号值；-1 = 位流中断；-2 = 非法码。
int decode_symbol(JpegBitReader &br, const JpegHuffmanTable &t) {
  int code = 0;
  for (int len = 1; len <= 16; len++) {
    const int bit = br.get_bits(1);
    if (bit < 0) return -1;
    code = (code << 1) | bit;
    if (t.max_code[len - 1] >= 0 && code <= t.max_code[len - 1]) {
      return t.symbols[t.val_offset[len - 1] + (code - t.min_code[len - 1])];
    }
  }
  return -2;
}

int extend(int bits, int size) {
  if (size <= 0) return 0;
  if (bits < (1 << (size - 1))) return bits - (1 << size) + 1;
  return bits;
}

// 解码一个 8x8 块。out 需为 64 个 float（自然序），返回 0 成功。
int decode_block(JpegBitReader &br, const JpegHuffmanTable &dc_t,
                 const JpegHuffmanTable &ac_t, float *out, int *dc_pred) {
  std::memset(out, 0, 64 * sizeof(float));
  // DC：差分 + 符号扩展
  const int t = decode_symbol(br, dc_t);
  if (t == -1) return -1;
  if (t == -2) return -2;
  int diff = 0;
  if (t > 0) {
    const int bits = br.get_bits(t);
    if (bits < 0) return -1;
    diff = extend(bits, t);
  }
  *dc_pred += diff;
  out[0] = static_cast<float>(*dc_pred);
  // AC：RLE（run/size 组合），zigzag 顺序
  int k = 1;
  while (k < 64) {
    const int rs = decode_symbol(br, ac_t);
    if (rs == -1) return -1;
    if (rs == -2) return -2;
    if (rs == 0x00) break; // EOB
    const int run = rs >> 4;
    const int size = rs & 15;
    if (size == 0) { // ZRL：16 个零
      k += 16;
      continue;
    }
    k += run;
    if (k >= 64) return -2; // 越界
    const int bits = br.get_bits(size);
    if (bits < 0) return -1;
    out[kJpegZigzag[k]] = static_cast<float>(extend(bits, size));
    k++;
  }
  return 0;
}

} // namespace

int jpeg_entropy_decode(const JpegImage &img, std::vector<float> *coeffs,
                        std::string *err) {
  coeffs->assign(static_cast<size_t>(img.total_blocks) * 64, 0.f);
  for (const JpegScanInfo &scan : img.scans) {
    JpegBitReader br(scan.entropy.data(), scan.entropy.size());
    int dc_pred[4] = {0, 0, 0, 0};

    int total_mcus = 0;
    if (scan.interleaved) {
      total_mcus = img.mcus_x * img.mcus_y;
    } else {
      const int ci = scan.comp[0].comp_index;
      total_mcus = img.comp[ci].blocks_x * img.comp[ci].blocks_y;
    }

    int mcu_expected = 0;
    for (int m = 0; m < total_mcus; m++) {
      if (img.restart_interval > 0 && mcu_expected == img.restart_interval) {
        if (!br.consume_restart()) {
          if (err) *err = "missing restart marker";
          return -1;
        }
        for (const auto &sc : scan.comp) dc_pred[sc.comp_index] = 0;
        mcu_expected = 0;
      }

      int block_result = 0;
      if (scan.interleaved) {
        const int mcx = m % img.mcus_x;
        const int mcy = m / img.mcus_x;
        for (const auto &sc : scan.comp) {
          const JpegComponentInfo &c = img.comp[sc.comp_index];
          for (int by = 0; by < c.v_samp; by++) {
            for (int bx = 0; bx < c.h_samp; bx++) {
              float *blk =
                  &(*coeffs)[c.coeff_off + ((mcy * c.v_samp + by) * c.blocks_x +
                                            mcx * c.h_samp + bx) *
                                               64];
              const int r =
                  decode_block(br, img.huff_dc[sc.td], img.huff_ac[sc.ta], blk,
                               &dc_pred[sc.comp_index]);
              if (r != 0) {
                block_result = r;
                break;
              }
            }
            if (block_result != 0) break;
          }
          if (block_result != 0) break;
        }
      } else {
        const JpegScanInfo::ScanComponent &sc = scan.comp[0];
        const JpegComponentInfo &c = img.comp[sc.comp_index];
        float *blk = &(*coeffs)[c.coeff_off + m * 64];
        block_result = decode_block(br, img.huff_dc[sc.td], img.huff_ac[sc.ta],
                                    blk, &dc_pred[sc.comp_index]);
      }

      if (block_result == -1) {
        // 非重启边界位流耗尽：文件截断或损坏。
        if (err) *err = "unexpected end of entropy data";
        return -1;
      }
      if (block_result == -2) {
        if (err) *err = "malformed huffman data";
        return -1;
      }
      mcu_expected++;
    }
  }
  return 0;
}

// ---- 编码 ----

namespace {

struct HuffEncodeEntry {
  uint16_t code = 0;
  uint8_t len = 0;
};

void build_encode_table(const JpegHuffmanTable &t, HuffEncodeEntry e[256]) {
  int code = 0;
  int si = 0;
  for (int len = 1; len <= 16; len++) {
    for (int i = 0; i < t.counts[len - 1]; i++) {
      const uint8_t sym = t.symbols[si++];
      e[sym].code = static_cast<uint16_t>(code);
      e[sym].len = static_cast<uint8_t>(len);
      code++;
    }
    code <<= 1;
  }
}

class JpegBitWriter {
public:
  explicit JpegBitWriter(std::vector<uint8_t> *out) : out_(out) {}
  void put_bits(int val, int n) {
    bitbuf_ =
        (bitbuf_ << n) | static_cast<uint32_t>(
                             val & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1)));
    bitcnt_ += n;
    while (bitcnt_ >= 8) {
      bitcnt_ -= 8;
      const uint8_t byte = static_cast<uint8_t>((bitbuf_ >> bitcnt_) & 0xFF);
      emit(byte);
    }
  }
  // 用 1 位填充到字节边界（JPEG 规范）。
  void flush() {
    while (bitcnt_ > 0) put_bits(1, 1);
  }

private:
  void emit(uint8_t b) {
    out_->push_back(b);
    if (b == 0xFF) out_->push_back(0x00); // 字节填充
  }
  std::vector<uint8_t> *out_;
  uint32_t bitbuf_ = 0;
  int bitcnt_ = 0;
};

int category(int v) {
  if (v == 0) return 0;
  int s = 0;
  int mag = v < 0 ? -v : v;
  while (mag) {
    s++;
    mag >>= 1;
  }
  return s;
}

void put_amplitude(JpegBitWriter &bw, int v, int s) {
  if (s <= 0) return;
  const int bits = (v >= 0) ? v : (v + (1 << s) - 1);
  bw.put_bits(bits, s);
}

// 量化系数在 DCT/量化阶段已经取整，以 float 存放。这里必须做**对称四舍五入**：
// `(int)(v + 0.5f)` 会把负数朝零截断（-5.0 -> -4），使所有负系数整体偏 1——
// AC 系数大约一半为负，实测这让 q=92 的 PSNR 掉约 5 dB，而且看起来不像 bug。
int roundCoeff(float v) { return static_cast<int>(std::lround(v)); }

void encode_block(JpegBitWriter &bw, const float *blk,
                  const HuffEncodeEntry *dc_e, const HuffEncodeEntry *ac_e,
                  int *prev_dc) {
  const int dc = roundCoeff(blk[0]);
  const int diff = dc - *prev_dc;
  *prev_dc = dc;
  const int sd = category(diff);
  bw.put_bits(dc_e[sd].code, dc_e[sd].len);
  put_amplitude(bw, diff, sd);

  int run = 0;
  for (int k = 1; k < 64; k++) {
    const int v = roundCoeff(blk[kJpegZigzag[k]]);
    if (v == 0) {
      run++;
      continue;
    }
    while (run >= 16) {
      bw.put_bits(ac_e[0xF0].code, ac_e[0xF0].len); // ZRL
      run -= 16;
    }
    const int s = category(v);
    const int rs = (run << 4) | s;
    bw.put_bits(ac_e[rs].code, ac_e[rs].len);
    put_amplitude(bw, v, s);
    run = 0;
  }
  if (run > 0) {
    bw.put_bits(ac_e[0x00].code, ac_e[0x00].len); // EOB
  }
}

} // namespace

int jpeg_entropy_encode(const JpegImage &img, const std::vector<float> &coeffs,
                        std::vector<uint8_t> *entropy, std::string *err) {
  HuffEncodeEntry dc_e[4][256];
  HuffEncodeEntry ac_e[4][256];
  bool dc_ok[4] = {false, false, false, false};
  bool ac_ok[4] = {false, false, false, false};
  for (int i = 0; i < 4; i++) {
    if (img.huff_dc[i].present) {
      build_encode_table(img.huff_dc[i], dc_e[i]);
      dc_ok[i] = true;
    }
    if (img.huff_ac[i].present) {
      build_encode_table(img.huff_ac[i], ac_e[i]);
      ac_ok[i] = true;
    }
  }

  entropy->clear();
  JpegBitWriter bw(entropy);
  int prev_dc[4] = {0, 0, 0, 0};

  // 单扫描交错 MCU 序。
  const int total_mcus = img.mcus_x * img.mcus_y;
  for (int m = 0; m < total_mcus; m++) {
    const int mcx = m % img.mcus_x;
    const int mcy = m / img.mcus_x;
    for (int ci = 0; ci < img.ncomp; ci++) {
      const JpegComponentInfo &c = img.comp[ci];
      if (!dc_ok[c.td] || !ac_ok[c.ta]) {
        if (err) *err = "missing huffman table for encoding";
        return -1;
      }
      for (int by = 0; by < c.v_samp; by++) {
        for (int bx = 0; bx < c.h_samp; bx++) {
          const float *blk =
              &coeffs[c.coeff_off + ((mcy * c.v_samp + by) * c.blocks_x +
                                     mcx * c.h_samp + bx) *
                                        64];
          encode_block(bw, blk, dc_e[c.td], ac_e[c.ta], &prev_dc[ci]);
        }
      }
    }
  }
  bw.flush();
  return 0;
}

} // namespace jpeg
} // namespace image_codec
