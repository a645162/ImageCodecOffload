/*
 * File: jpeg_bitstream.cpp
 * Brief: Jpeg bitstream.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/jpeg_bitstream.cpp - baseline JPEG 容器解析与组帧实现。
#include "jpeg_bitstream.h"

#include <cstring>

namespace image_codec {
namespace jpeg {

// 标准 zigzag 顺序：zigzag[i] 给出自然序（行优先）中的位置。
const uint8_t kJpegZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10,
    17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

// ---- Huffman 解码表 ----

void jpeg_build_decode_table(JpegHuffmanTable* t) {
  int code = 0;
  int k = 0;
  for (int len = 1; len <= 16; len++) {
    const int count = t->counts[len - 1];
    if (count > 0) {
      t->min_code[len - 1] = code;
      t->max_code[len - 1] = code + count - 1;
      t->val_offset[len - 1] = k;
      k += count;
      code = (code + count) << 1;
    } else {
      t->min_code[len - 1] = -1;
      t->max_code[len - 1] = -1;
      t->val_offset[len - 1] = 0;
      code <<= 1;  // 空码长也必须移位（规范码连续编号）
    }
  }
  t->dec_ready = true;
}

// ---- 标准 Huffman 表（Annex K.3）----

static const JpegHuffmanTable kStdDcLum = {
    {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0},
    12, true};

static const JpegHuffmanTable kStdDcChroma = {
    {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0},
    12, true};

static const JpegHuffmanTable kStdAcLum = {
    {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
     0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
     0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
     0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
     0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
     0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
     0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
     0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
     0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
     0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
     0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
     0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
     0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
     0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa},
    162, true};

static const JpegHuffmanTable kStdAcChroma = {
    {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
     0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
     0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
     0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
     0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
     0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
     0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
     0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
     0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
     0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
     0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
     0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
     0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
     0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa},
    162, true};

const JpegHuffmanTable& jpeg_std_dc_lum() { return kStdDcLum; }
const JpegHuffmanTable& jpeg_std_dc_chroma() { return kStdDcChroma; }
const JpegHuffmanTable& jpeg_std_ac_lum() { return kStdAcLum; }
const JpegHuffmanTable& jpeg_std_ac_chroma() { return kStdAcChroma; }

// ---- 布局计算 ----

void jpeg_compute_layout(JpegImage* img) {
  const int mw = img->max_h * 8;
  const int mh = img->max_v * 8;
  img->mcus_x = (img->width + mw - 1) / mw;
  img->mcus_y = (img->height + mh - 1) / mh;
  int coeff_cum = 0;
  int plane_cum = 0;
  img->total_blocks = 0;
  for (int c = 0; c < img->ncomp; c++) {
    JpegComponentInfo& ci = img->comp[c];
    ci.plane_w = (img->width * ci.h_samp + img->max_h - 1) / img->max_h;
    ci.plane_h = (img->height * ci.v_samp + img->max_v - 1) / img->max_v;
    ci.blocks_x = img->mcus_x * ci.h_samp;
    ci.blocks_y = img->mcus_y * ci.v_samp;
    const int blocks = ci.blocks_x * ci.blocks_y;
    ci.coeff_off = coeff_cum * 64;
    ci.plane_off = plane_cum;
    coeff_cum += blocks;
    plane_cum += (ci.blocks_x * 8) * (ci.blocks_y * 8);
    img->total_blocks += blocks;
  }
}

// ---- 量化表 ----

static const uint8_t kBaseLum[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
};

static const uint8_t kBaseChroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

void jpeg_make_quant_tables(int quality, JpegQuantTable out[2]) {
  if (quality < 1) quality = 1;
  if (quality > 100) quality = 100;
  const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
  const uint8_t* bases[2] = {kBaseLum, kBaseChroma};
  for (int t = 0; t < 2; t++) {
    for (int i = 0; i < 64; i++) {
      int v = (bases[t][i] * scale + 50) / 100;
      if (v < 1) v = 1;
      if (v > 255) v = 255;
      out[t].q[i] = static_cast<uint16_t>(v);
    }
    out[t].present = true;
  }
}

// ---- 解析 ----

static uint16_t rd_u16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static void fail(std::string* err, const std::string& msg, int rc = -1) {
  if (err) *err = msg;
  (void)rc;
}

int jpeg_parse(const std::vector<uint8_t>& file, JpegImage* img, std::string* err) {
  img->valid = false;
  img->progressive = false;
  img->scans.clear();
  img->raw = file;
  const size_t size = file.size();
  if (size < 4 || file[0] != 0xFF || file[1] != 0xD8) {
    fail(err, "not a JPEG file (missing SOI)");
    return -1;
  }
  size_t pos = 2;
  bool saw_sof = false;
  bool saw_sos = false;

  while (pos < size) {
    if (file[pos] != 0xFF) {
      fail(err, "expected marker at offset");
      return -1;
    }
    pos++;
    if (pos >= size) {
      fail(err, "truncated file (marker only)");
      return -1;
    }
    const uint8_t m = file[pos++];

    if (m == 0xD9) {  // EOI
      break;
    }
    if (m == 0x01) continue;  // TEM，无长度
    if (m >= 0xD0 && m <= 0xD7) continue;  // RSTn（扫描外不应出现，跳过）

    if (pos + 2 > size) {
      fail(err, "truncated segment length");
      return -1;
    }
    const uint16_t len = rd_u16(&file[pos]);
    pos += 2;
    if (len < 2 || pos + (len - 2) > size) {
      fail(err, "bad segment length");
      return -1;
    }
    const size_t seg_end = pos + (len - 2);

    if (m == 0xC0) {  // SOF0
      if (saw_sof) {
        fail(err, "multiple SOF not supported");
        return -1;
      }
      const int precision = file[pos];
      if (precision != 8) {
        fail(err, "only 8-bit baseline JPEG supported");
        return -1;
      }
      img->height = rd_u16(&file[pos + 1]);
      img->width = rd_u16(&file[pos + 3]);
      const int ncomp = file[pos + 5];
      if (ncomp < 1 || ncomp > 4) {
        fail(err, "unsupported component count");
        return -1;
      }
      img->ncomp = ncomp;
      img->max_h = 0;
      img->max_v = 0;
      size_t p = pos + 6;
      for (int c = 0; c < ncomp; c++) {
        if (p + 3 > seg_end) {
          fail(err, "truncated SOF");
          return -1;
        }
        JpegComponentInfo& ci = img->comp[c];
        ci = JpegComponentInfo();
        ci.id = file[p];
        const uint8_t hv = file[p + 1];
        ci.h_samp = hv >> 4;
        ci.v_samp = hv & 15;
        ci.tq = file[p + 2];
        p += 3;
        if (ci.h_samp == 0 || ci.v_samp == 0) {
          fail(err, "zero sampling factor");
          return -1;
        }
        img->max_h = img->max_h > ci.h_samp ? img->max_h : ci.h_samp;
        img->max_v = img->max_v > ci.v_samp ? img->max_v : ci.v_samp;
      }
      if (img->max_h > 4 || img->max_v > 4) {
        fail(err, "sampling factors > 4 not supported");
        return -1;
      }
      jpeg_compute_layout(img);
      saw_sof = true;
    } else if (m == 0xC2) {  // SOF2 progressive
      fail(err, "progressive JPEG (SOF2) not supported");
      return -1;
    } else if (m == 0xC4) {  // DHT
      size_t p = pos;
      while (p + 17 <= seg_end) {
        const uint8_t b = file[p++];
        const int tc = b >> 4;
        const int th = b & 15;
        if (th > 3) {
          fail(err, "huffman table index > 3");
          return -1;
        }
        JpegHuffmanTable t;
        uint16_t total = 0;
        for (int i = 0; i < 16; i++) {
          t.counts[i] = file[p++];
          total += t.counts[i];
        }
        if (total > 256 || p + total > seg_end) {
          fail(err, "bad DHT table");
          return -1;
        }
        for (int i = 0; i < total; i++) {
          t.symbols[i] = file[p++];
        }
        t.nsymbols = static_cast<uint8_t>(total);
        t.present = true;
        jpeg_build_decode_table(&t);
        if (tc == 0) {
          img->huff_dc[th] = t;
        } else {
          img->huff_ac[th] = t;
        }
      }
    } else if (m == 0xC1 || (m >= 0xC3 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCF)) {
      fail(err, "only baseline SOF0 supported");
      return -1;
    } else if (m == 0xDB) {  // DQT
      size_t p = pos;
      while (p + 65 <= seg_end) {
        const uint8_t b = file[p++];
        const int pq = b >> 4;
        const int tq = b & 15;
        if (pq != 0) {
          fail(err, "16-bit quant tables not supported");
          return -1;
        }
        if (tq > 3) {
          fail(err, "quant table index > 3");
          return -1;
        }
        JpegQuantTable& t = img->quant[tq];
        for (int i = 0; i < 64; i++) {
          t.q[kJpegZigzag[i]] = file[p++];  // 文件内为 zigzag 序，转自然序
        }
        t.present = true;
      }
    } else if (m == 0xDD) {  // DRI
      img->restart_interval = rd_u16(&file[pos]);
    } else if (m == 0xDA) {  // SOS
      if (!saw_sof) {
        fail(err, "SOS before SOF");
        return -1;
      }
      JpegScanInfo scan;
      size_t p = pos;
      const int n = file[p++];
      if (n < 1 || n > 4 || p + 2u * n + 3 > seg_end) {
        fail(err, "bad SOS header");
        return -1;
      }
      for (int i = 0; i < n; i++) {
        const int id = file[p++];
        const uint8_t t = file[p++];
        int ci = -1;
        for (int k = 0; k < img->ncomp; k++) {
          if (img->comp[k].id == static_cast<uint8_t>(id)) {
            ci = k;
            break;
          }
        }
        if (ci < 0) {
          fail(err, "SOS references unknown component");
          return -1;
        }
        JpegScanInfo::ScanComponent sc;
        sc.comp_index = ci;
        sc.td = t >> 4;
        sc.ta = t & 15;
        scan.comp.push_back(sc);
        img->comp[ci].td = static_cast<uint8_t>(sc.td);
        img->comp[ci].ta = static_cast<uint8_t>(sc.ta);
      }
      scan.s0 = file[p++];
      scan.s1 = file[p++];
      const uint8_t aa = file[p++];
      scan.ah = aa >> 4;
      scan.al = aa & 15;
      if (scan.s0 != 0 || scan.s1 != 63 || scan.ah != 0 || scan.al != 0) {
        fail(err, "only baseline spectral selection (Ss=0,Se=63) supported");
        return -1;
      }
      scan.interleaved = (n > 1);

      // 扫描熵数据：直到下一个非 RST 标记（从 SOS 头部之后开始）
      size_t e0 = p;
      while (p < size) {
        if (file[p] == 0xFF) {
          if (p + 1 >= size) break;
          const uint8_t b2 = file[p + 1];
          if (b2 == 0x00) {
            p += 2;
            continue;
          }
          if (b2 >= 0xD0 && b2 <= 0xD7) {
            p += 2;
            continue;
          }
          break;  // 真实标记结束扫描
        }
        p++;
      }
      scan.entropy.assign(file.begin() + static_cast<std::ptrdiff_t>(e0),
                          file.begin() + static_cast<std::ptrdiff_t>(p));
      img->scans.push_back(scan);
      saw_sos = true;
      pos = p;  // 指向终止标记的 0xFF，继续外层循环
      continue;
    }
    // APPn/COM/其他：跳过
    pos = seg_end;
  }

  if (!saw_sof || img->scans.empty()) {
    fail(err, "no SOF0/SOS found");
    return -1;
  }
  img->valid = true;
  return 0;
}

// ---- 组帧写出 ----

static void put_u16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v >> 8));
  out->push_back(static_cast<uint8_t>(v & 0xFF));
}

static void emit_dht(std::vector<uint8_t>* out, const JpegHuffmanTable& t,
                     int tc, int th) {
  out->push_back(0xFF);
  out->push_back(0xC4);
  const size_t len_pos = out->size();
  put_u16(out, 0);
  out->push_back(static_cast<uint8_t>((tc << 4) | th));
  for (int i = 0; i < 16; i++) out->push_back(t.counts[i]);
  for (int i = 0; i < t.nsymbols; i++) out->push_back(t.symbols[i]);
  const uint16_t len = static_cast<uint16_t>(out->size() - len_pos);
  (*out)[len_pos] = static_cast<uint8_t>(len >> 8);
  (*out)[len_pos + 1] = static_cast<uint8_t>(len & 0xFF);
}

static void emit_dqt(std::vector<uint8_t>* out, const JpegQuantTable& t, int tq) {
  out->push_back(0xFF);
  out->push_back(0xDB);
  const size_t len_pos = out->size();
  put_u16(out, 0);
  out->push_back(static_cast<uint8_t>(tq));  // pq=0
  for (int i = 0; i < 64; i++) {
    out->push_back(static_cast<uint8_t>(t.q[kJpegZigzag[i]]));
  }
  const uint16_t len = static_cast<uint16_t>(out->size() - len_pos);
  (*out)[len_pos] = static_cast<uint8_t>(len >> 8);
  (*out)[len_pos + 1] = static_cast<uint8_t>(len & 0xFF);
}

int jpeg_write_file(const JpegImage& img, const std::vector<uint8_t>& entropy,
                    std::vector<uint8_t>* out, std::string* err) {
  out->clear();
  // SOI
  out->push_back(0xFF);
  out->push_back(0xD8);
  // APP0 JFIF 1.01
  out->push_back(0xFF);
  out->push_back(0xE0);
  put_u16(out, 16);
  const uint8_t jfif[14] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
  out->insert(out->end(), jfif, jfif + 14);
  // DQT：收集被引用的表
  bool used_q[4] = {false, false, false, false};
  for (int c = 0; c < img.ncomp; c++) used_q[img.comp[c].tq] = true;
  for (int i = 0; i < 4; i++) {
    if (used_q[i] && img.quant[i].present) emit_dqt(out, img.quant[i], i);
  }
  // DHT：收集被引用的表
  bool used_dc[4] = {false, false, false, false};
  bool used_ac[4] = {false, false, false, false};
  for (int c = 0; c < img.ncomp; c++) {
    used_dc[img.comp[c].td] = true;
    used_ac[img.comp[c].ta] = true;
  }
  for (int i = 0; i < 4; i++) {
    if (used_dc[i] && img.huff_dc[i].present) {
      JpegHuffmanTable t = img.huff_dc[i];
      t.present = true;
      emit_dht(out, t, 0, i);
    }
    if (used_ac[i] && img.huff_ac[i].present) {
      JpegHuffmanTable t = img.huff_ac[i];
      t.present = true;
      emit_dht(out, t, 1, i);
    }
  }
  // SOF0
  out->push_back(0xFF);
  out->push_back(0xC0);
  put_u16(out, static_cast<uint16_t>(8 + 3 * img.ncomp));
  out->push_back(8);  // 精度
  put_u16(out, static_cast<uint16_t>(img.height));
  put_u16(out, static_cast<uint16_t>(img.width));
  out->push_back(static_cast<uint8_t>(img.ncomp));
  for (int c = 0; c < img.ncomp; c++) {
    const JpegComponentInfo& ci = img.comp[c];
    out->push_back(ci.id);
    out->push_back(static_cast<uint8_t>((ci.h_samp << 4) | ci.v_samp));
    out->push_back(ci.tq);
  }
  // SOS（单扫描交错）
  out->push_back(0xFF);
  out->push_back(0xDA);
  put_u16(out, static_cast<uint16_t>(6 + 2 * img.ncomp));
  out->push_back(static_cast<uint8_t>(img.ncomp));
  for (int c = 0; c < img.ncomp; c++) {
    const JpegComponentInfo& ci = img.comp[c];
    out->push_back(ci.id);
    out->push_back(static_cast<uint8_t>((ci.td << 4) | ci.ta));
  }
  out->push_back(0);  // Ss
  out->push_back(63);  // Se
  out->push_back(0);  // Ah/Al
  // 熵数据（已含字节填充）
  out->insert(out->end(), entropy.begin(), entropy.end());
  // EOI
  out->push_back(0xFF);
  out->push_back(0xD9);
  return 0;
}

}  // namespace jpeg
}  // namespace image_codec
