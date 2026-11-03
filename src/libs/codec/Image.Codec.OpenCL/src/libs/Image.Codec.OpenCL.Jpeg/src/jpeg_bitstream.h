/*
 * File: jpeg_bitstream.h
 * Brief: Jpeg bitstream.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/jpeg_bitstream.h - baseline JPEG 容器解析与组帧（SOI/APPn/DQT/DHT/SOF0/SOS/DRI/EOI）。
// 混合编解码策略：本模块只负责容器（marker/segment/表/MCU 布局）与文件写出，
// 熵编解码见 jpeg_huffman.*，DCT/颜色变换见 jpeg_dct_ocl.*。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace image_codec {
namespace jpeg {

// 标准 zigzag 顺序表：kJpegZigzag[i] = 自然序(行优先 8x8)索引。
extern const uint8_t kJpegZigzag[64];

// 量化表（自然序 q[64]，q[0] 为 DC）。
struct JpegQuantTable {
  uint16_t q[64] = {0};
  bool present = false;
};

// Huffman 表。counts/symbols 为 JPEG 定义的标准形式（16 个码长计数 + 符号列表）。
// 解码前需调用 jpeg_build_decode_table() 填充 min_code/max_code/val_offset。
struct JpegHuffmanTable {
  uint8_t counts[16] = {0};
  uint8_t symbols[256] = {0};
  uint8_t nsymbols = 0;
  bool present = false;

  // 解码用（由 jpeg_build_decode_table 填充）。注意：len16 规范码值可达 65534，
  // 必须用 32 位存储，不能是 int16。
  int min_code[16];
  int max_code[16];
  int val_offset[16];
  bool dec_ready = false;
};

// 帧分量（SOF0）+ 扫描分量（SOS）信息 + 布局。
struct JpegComponentInfo {
  uint8_t id = 0;          // SOF/SOS 中的分量 id
  uint8_t h_samp = 1;      // 水平采样因子
  uint8_t v_samp = 1;      // 垂直采样因子
  uint8_t tq = 0;          // 量化表选择（SOF）
  uint8_t td = 0;          // DC Huffman 表选择（SOS）
  uint8_t ta = 0;          // AC Huffman 表选择（SOS）

  // 布局（由 jpeg_compute_layout 计算）：
  int plane_w = 0;         // 该分量有效像素宽（含向上取整）
  int plane_h = 0;         // 该分量有效像素高
  int blocks_x = 0;        // 8x8 块数（含补齐到 MCU 网格的部分）
  int blocks_y = 0;
  int coeff_off = 0;       // 组合系数缓冲区偏移（单位 float，64*块数）
  int plane_off = 0;       // 组合平面缓冲区偏移（单位 float）
};

// 一个 SOS 扫描。
struct JpegScanInfo {
  struct ScanComponent {
    int comp_index = 0;    // 指向 JpegImage::comp[]
    int td = 0, ta = 0;
  };
  std::vector<ScanComponent> comp;
  std::vector<uint8_t> entropy;  // 该扫描的熵编码数据（原始字节，含 0xFF00 填充与 RST 标记）
  int s0 = 0, s1 = 63;           // 谱选择（baseline 恒为 0,63）
  int ah = 0, al = 0;            // 逐位近似（baseline 恒为 0,0）
  bool interleaved = false;      // comp.size() > 1
};

// 解析后的完整图像（解码/编码共用）。
struct JpegImage {
  int width = 0, height = 0;
  int ncomp = 0;
  int max_h = 1, max_v = 1;      // 最大采样因子
  int mcus_x = 0, mcus_y = 0;    // MCU 网格
  int total_blocks = 0;          // 全部分量块数之和
  JpegComponentInfo comp[4];
  JpegQuantTable quant[4];
  JpegHuffmanTable huff_dc[4], huff_ac[4];
  int restart_interval = 0;      // DRI
  std::vector<JpegScanInfo> scans;
  bool progressive = false;
  bool valid = false;
  std::vector<uint8_t> raw;      // 原始文件字节（解析时保留，供诊断）
};

// 由宽高/采样因子计算 MCU 网格与各分量布局。
void jpeg_compute_layout(JpegImage* img);

// 解析 JPEG 文件字节。成功返回 0。
int jpeg_parse(const std::vector<uint8_t>& file, JpegImage* img, std::string* err);

// 为解码预计算 Huffman 码表。
void jpeg_build_decode_table(JpegHuffmanTable* t);

// 标准表（Annex K.3）。
const JpegHuffmanTable& jpeg_std_dc_lum();
const JpegHuffmanTable& jpeg_std_dc_chroma();
const JpegHuffmanTable& jpeg_std_ac_lum();
const JpegHuffmanTable& jpeg_std_ac_chroma();

// 按质量(1..100)生成标准量化表（Annex K.1/K.2 缩放），out[0]=亮度,out[1]=色度。
void jpeg_make_quant_tables(int quality, JpegQuantTable out[2]);

// 组帧写出：SOI + APP0(JFIF) + DQT + DHT + SOF0 + SOS + 熵数据 + EOI。
// entropy 为熵编码后的字节流（已含字节填充）。成功返回 0。
int jpeg_write_file(const JpegImage& img, const std::vector<uint8_t>& entropy,
                    std::vector<uint8_t>* out, std::string* err);

}  // namespace jpeg
}  // namespace image_codec
