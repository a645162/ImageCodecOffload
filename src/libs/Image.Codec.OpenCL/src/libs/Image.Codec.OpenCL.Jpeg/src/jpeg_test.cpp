/*
 * File: jpeg_test.cpp
 * Brief: Jpeg test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/jpeg_test.cpp - JPEG CLI 测试入口实现。
#include "jpeg_test.h"

#include "bmp_io.h"
#include "jpeg_bitstream.h"
#include "jpeg_codec.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int read_file(const char *path, std::vector<uint8_t> *out) {
  FILE *f = std::fopen(path, "rb");
  if (!f) return -1;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out->insert(out->end(), buf, buf + n);
  }
  std::fclose(f);
  return 0;
}

} // namespace

extern "C" {

int image_codec_jpeg_info(const char *path) {
  if (!path) return -1;
  std::vector<uint8_t> file;
  if (read_file(path, &file) != 0) {
    std::fprintf(stderr, "image_codec_jpeg_info: cannot read %s\n", path);
    return -1;
  }
  image_codec::jpeg::JpegImage img;
  std::string err;
  if (image_codec::jpeg::jpeg_parse(file, &img, &err) != 0) {
    std::fprintf(stderr, "image_codec_jpeg_info: %s\n", err.c_str());
    return -1;
  }
  std::printf("JPEG: %dx%d, %d component(s), progressive=%d\n", img.width,
              img.height, img.ncomp, img.progressive ? 1 : 0);
  for (int c = 0; c < img.ncomp; c++) {
    const auto &ci = img.comp[c];
    std::printf("  comp[%d] id=%u sampling=%ux%u tq=%u td=%u ta=%u plane=%dx%d "
                "blocks=%dx%d\n",
                c, ci.id, ci.h_samp, ci.v_samp, ci.tq, ci.td, ci.ta, ci.plane_w,
                ci.plane_h, ci.blocks_x, ci.blocks_y);
  }
  std::printf("  MCU grid: %dx%d, max_samp=%ux%u, restart_interval=%d\n",
              img.mcus_x, img.mcus_y, img.max_h, img.max_v,
              img.restart_interval);
  for (int t = 0; t < 4; t++) {
    if (img.quant[t].present) {
      std::printf("  quant[%d]: DC=%u, AC[1]=%u\n", t, img.quant[t].q[0],
                  img.quant[t].q[1]);
    }
  }
  for (int t = 0; t < 4; t++) {
    if (img.huff_dc[t].present)
      std::printf("  huff_dc[%d]: %d symbols\n", t, img.huff_dc[t].nsymbols);
    if (img.huff_ac[t].present)
      std::printf("  huff_ac[%d]: %d symbols\n", t, img.huff_ac[t].nsymbols);
  }
  return 0;
}

int image_codec_jpeg_decode(const char *in, const char *out_bmp, int backend) {
  if (!in || !out_bmp) return -1;
  image_codec::jpeg::JpegDecodeResult res;
  if (image_codec::jpeg::jpeg_decode_file(in, backend, &res) != 0) {
    std::fprintf(stderr, "image_codec_jpeg_decode: %s\n", res.error.c_str());
    return -1;
  }
  std::string err;
  if (image_codec::bmp_write_pixels(out_bmp, res.rgb.data(), res.width,
                                    res.height, res.channels, &err) != 0) {
    std::fprintf(stderr, "image_codec_jpeg_decode: %s\n", err.c_str());
    return -1;
  }
  std::printf("decoded %s -> %s (%dx%d, subsampling=%d)\n", in, out_bmp,
              res.width, res.height, res.subsampling);
  return 0;
}

int image_codec_jpeg_encode(const char *in_bmp, const char *out, int backend,
                            int quality, int subsampling) {
  if (!in_bmp || !out) return -1;
  std::string err;
  if (image_codec::jpeg::jpeg_encode_bmp_file(in_bmp, out, backend, quality,
                                              subsampling, &err) != 0) {
    std::fprintf(stderr, "image_codec_jpeg_encode: %s\n", err.c_str());
    return -1;
  }
  std::printf("encoded %s -> %s (quality=%d, subsampling=%d, backend=%d)\n",
              in_bmp, out, quality, subsampling, backend);
  return 0;
}

} // extern "C"
