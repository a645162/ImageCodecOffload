/*
 * File: selftest.cpp
 * Brief: Selftest.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/selftest/selftest.cpp - JPEG 模块自测（临时，默认关闭）。
// 用法: image_codec_jpeg_selftest <workdir>
//   workdir 内需有由 gen_ref.py 生成的：
//     ref.bmp, ref_444_q95.jpg, ref_444_pil.bmp, ref_420_q90.jpg, ref_420_pil.bmp,
//     ref_422_q90.jpg, ref_422_pil.bmp, gray_q95.jpg, gray_pil.bmp, tiny8_q95.jpg
// 输出解码与参考(BMP 像素)的 PSNR/max-diff，以及 encode->decode roundtrip PSNR。
#include "bmp_io.h"
#include "jpeg_bitstream.h"
#include "jpeg_codec.h"
#include "jpeg_test.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace image_codec::jpeg;

namespace {

struct Metrics {
  double max_diff = 0.0;
  double psnr = 0.0;
  int width = 0, height = 0;
  int diff_count_gt1 = 0;
  int diff_count_gt3 = 0;
};

// 逐通道比较（RGB）。
Metrics compare_rgb(const uint8_t* a, const uint8_t* b, int w, int h) {
  Metrics m;
  m.width = w;
  m.height = h;
  double sse = 0.0;
  const size_t n = static_cast<size_t>(w) * h * 3;
  for (size_t i = 0; i < n; i++) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    if (d < 0) d = -d;
    if (d > m.max_diff) m.max_diff = d;
    if (d > 1) m.diff_count_gt1++;
    if (d > 3) m.diff_count_gt3++;
    sse += d * d;
  }
  const double mse = sse / static_cast<double>(w * h * 3);
  m.psnr = (mse <= 1e-10) ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
  return m;
}

bool load_file(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out->insert(out->end(), buf, buf + n);
  }
  std::fclose(f);
  return true;
}

void report(const char* name, const Metrics& m) {
  std::printf("%-28s  PSNR=%6.2f dB  max_diff=%3.0f  diff>1:%6d  diff>3:%6d  (%dx%d)\n",
              name, m.psnr, m.max_diff, m.diff_count_gt1, m.diff_count_gt3, m.width,
              m.height);
}

int decode_compare(const std::string& jpg, const std::string& ref_bmp,
                   const char* name) {
  std::vector<uint8_t> file, ref;
  if (!load_file(jpg, &file) || !load_file(ref_bmp, &ref)) {
    std::printf("SKIP %-25s (missing input: '%s' '%s')\n", name, jpg.c_str(),
                ref_bmp.c_str());
    return -1;
  }
  int ref_w = 0, ref_h = 0;
  std::string err;
  std::vector<uint8_t> ref_rgb;
  if (bmp_read(ref_bmp, &ref_rgb, &ref_w, &ref_h, &err) != 0) {
    std::printf("SKIP %-25s (bad ref bmp: %s)\n", name, err.c_str());
    return -1;
  }
  int results[2] = {0, 0};
  for (int backend = 0; backend <= 1; backend++) {
    JpegDecodeResult res;
    if (jpeg_decode_buffer(file, backend, &res) != 0) {
      std::printf("FAIL %-25s backend=%d: %s\n", name, backend, res.error.c_str());
      return 1;
    }
    if (res.width != ref_w || res.height != ref_h) {
      std::printf("FAIL %-25s backend=%d: size %dx%d != ref %dx%d\n", name, backend,
                  res.width, res.height, ref_w, ref_h);
      return 1;
    }
    Metrics m = compare_rgb(res.rgb.data(), ref_rgb.data(), ref_w, ref_h);
    std::printf("%-20s backend=%s  ", name, backend == 0 ? "opencl" : "cpu   ");
    report("", m);
    results[backend] = (m.max_diff <= 3.0) ? 0 : 1;
  }
  return (results[0] == 0 && results[1] == 0) ? 0 : 1;
}

int roundtrip(const std::string& bmp, int quality, const char* name) {
  std::vector<uint8_t> rgb;
  int w = 0, h = 0;
  std::string err;
  if (bmp_read(bmp, &rgb, &w, &h, &err) != 0) {
    std::printf("SKIP %-25s (%s)\n", name, err.c_str());
    return -1;
  }
  std::vector<uint8_t> jpg;
  if (jpeg_encode_buffer(rgb, w, h, quality, 0, 0, &jpg, &err) != 0) {
    std::printf("FAIL %-25s encode: %s\n", name, err.c_str());
    return 1;
  }
  JpegDecodeResult res;
  if (jpeg_decode_buffer(jpg, 0, &res) != 0) {
    std::printf("FAIL %-25s decode: %s\n", name, res.error.c_str());
    return 1;
  }
  Metrics m = compare_rgb(rgb.data(), res.rgb.data(), w, h);
  std::printf("%-20s encode->decode(q=%d)  ", name, quality);
  report("", m);
  return (m.psnr >= 40.0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = (argc > 1) ? argv[1] : ".";
  const auto p = [&](const char* f) { return dir + "/" + f; };

  std::printf("=== JPEG 模块自测 (workdir=%s) ===\n", dir.c_str());

  int rc = 0;
  // 解码正确性：我的解码 vs libjpeg(PIL) 解码
  rc |= decode_compare(p("ref_444_q95.jpg"), p("ref_444_q95_pil.bmp"),
                       "decode 4:4:4 vs libjpeg");
  rc |= decode_compare(p("ref_420_q90.jpg"), p("ref_420_q90_pil.bmp"),
                       "decode 4:2:0 vs libjpeg");
  rc |= decode_compare(p("ref_422_q90.jpg"), p("ref_422_q90_pil.bmp"),
                       "decode 4:2:2 vs libjpeg");
  rc |= decode_compare(p("gray_q95.jpg"), p("gray_q95_pil.bmp"),
                       "decode grayscale vs libjpeg");
  rc |= decode_compare(p("tiny8_q95.jpg"), p("tiny8_q95_pil.bmp"),
                       "decode 8x8 4:4:4 vs libjpeg");

  // 解码正确性：我的解码 vs 原始图（PIL 编码有损，仅参考）
  decode_compare(p("ref_444_q95.jpg"), p("ref.bmp"),
                 "decode 4:4:4 vs original [ref]");
  decode_compare(p("ref_420_q90.jpg"), p("ref.bmp"),
                 "decode 4:2:0 vs original [ref]");

  // 编码 roundtrip
  rc |= roundtrip(p("ref.bmp"), 100, "roundtrip 4:4:4 q100");
  rc |= roundtrip(p("ref.bmp"), 97, "roundtrip 4:4:4 q97");
  roundtrip(p("ref.bmp"), 92, "roundtrip 4:4:4 q92 [info]");
  rc |= roundtrip(p("tiny8.bmp"), 90, "roundtrip 8x8 q90");

  // 固定签名 CLI 入口（供上层集成）自检
  const std::string cli_in = p("ref_444_q95.jpg");
  const std::string cli_out = dir + "/cli_dec.bmp";
  const std::string cli_bmp = p("ref.bmp");
  const std::string cli_jpg = dir + "/cli_enc.jpg";
  std::printf("[cli] image_codec_jpeg_info: ");
  const int r_info = image_codec_jpeg_info(cli_in.c_str());
  std::printf("[cli] image_codec_jpeg_decode -> %d\n", image_codec_jpeg_decode(cli_in.c_str(), cli_out.c_str(), 0));
  std::printf("[cli] image_codec_jpeg_encode -> %d\n", image_codec_jpeg_encode(cli_bmp.c_str(), cli_jpg.c_str(), 0, 95));
  if (r_info != 0) rc |= 1;

  std::printf("=== %s ===\n", rc == 0 ? "ALL PASS" : "FAILURES PRESENT");
  return rc;
}
