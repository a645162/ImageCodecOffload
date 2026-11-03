/*
 * File: onevpl_test.cpp
 * Brief: Onevpl test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// onevpl_test.cpp - oneVPL JPEG 后端的 C 入口实现（BMP 中转 + 探测）。
#include "onevpl_test.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bmp24.h"
#include "vpl_device.h"
#include "vpl_dyn.h"
#include "vpl_jpeg_decoder.h"
#include "vpl_jpeg_encoder.h"

namespace {

// BGRA32 -> BGR24（紧致）。
void bgra_to_bgr(const std::vector<uint8_t> &bgra, std::vector<uint8_t> &bgr,
                 size_t pixels) {
  bgr.assign(pixels * 3, 0);
  for (size_t i = 0; i < pixels; ++i) {
    bgr[i * 3 + 0] = bgra[i * 4 + 0];
    bgr[i * 3 + 1] = bgra[i * 4 + 1];
    bgr[i * 3 + 2] = bgra[i * 4 + 2];
  }
}

} // namespace

extern "C" {

int oic_onevpl_encode(const char *in_bmp, const char *out_jpeg, int quality) {
  using namespace oic::jpeg::onevpl;
  if (in_bmp == nullptr || out_jpeg == nullptr) {
    std::fprintf(stderr, "[onevpl] encode: 参数为空\n");
    return 1;
  }
  std::string error;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> bgr;
  if (!bmp_load_24(in_bmp, width, height, bgr, error)) {
    std::fprintf(stderr, "[onevpl] encode: %s\n", error.c_str());
    return 1;
  }

  VplJpegEncoder encoder;
  if (!encoder.init(width, height, static_cast<uint32_t>(quality))) {
    std::fprintf(stderr, "[onevpl] encode: %s\n", encoder.last_error.c_str());
    return 1;
  }
  std::vector<uint8_t> jpeg;
  const bool ok = encoder.encode_bgr24(bgr.data(), bgr.size(), jpeg);
  const std::string impl = encoder.impl_name;
  encoder.close();
  if (!ok || jpeg.empty()) {
    std::fprintf(stderr, "[onevpl] encode: 编码失败\n");
    return 1;
  }

  FILE *file = std::fopen(out_jpeg, "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "[onevpl] encode: 无法写入 %s\n", out_jpeg);
    return 1;
  }
  std::fwrite(jpeg.data(), 1, jpeg.size(), file);
  std::fclose(file);
  std::printf("[onevpl] encode ok: %ux%u q=%d -> %s (%zu bytes, impl=%s)\n",
              width, height, quality, out_jpeg, jpeg.size(), impl.c_str());
  return 0;
}

int oic_onevpl_decode(const char *in_jpeg, const char *out_bmp) {
  using namespace oic::jpeg::onevpl;
  if (in_jpeg == nullptr || out_bmp == nullptr) {
    std::fprintf(stderr, "[onevpl] decode: 参数为空\n");
    return 1;
  }
  std::vector<uint8_t> jpeg;
  {
    FILE *file = std::fopen(in_jpeg, "rb");
    if (file == nullptr) {
      std::fprintf(stderr, "[onevpl] decode: 无法打开 %s\n", in_jpeg);
      return 1;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
      std::fclose(file);
      std::fprintf(stderr, "[onevpl] decode: 文件为空 %s\n", in_jpeg);
      return 1;
    }
    jpeg.resize(static_cast<size_t>(size));
    const bool read_ok =
        std::fread(jpeg.data(), 1, jpeg.size(), file) == jpeg.size();
    std::fclose(file);
    if (!read_ok) {
      std::fprintf(stderr, "[onevpl] decode: 读取失败 %s\n", in_jpeg);
      return 1;
    }
  }

  VplJpegDecoder decoder;
  if (!decoder.init()) {
    std::fprintf(stderr, "[onevpl] decode: %s\n", decoder.last_error.c_str());
    return 1;
  }
  std::vector<uint8_t> bgra;
  uint32_t width = 0;
  uint32_t height = 0;
  const bool ok = decoder.decode(jpeg.data(), jpeg.size(), bgra, width, height);
  const std::string impl = decoder.impl_name;
  decoder.close();
  if (!ok || bgra.empty()) {
    std::fprintf(stderr, "[onevpl] decode: 解码失败\n");
    return 1;
  }

  std::vector<uint8_t> bgr;
  bgra_to_bgr(bgra, bgr, static_cast<size_t>(width) * height);
  std::string error;
  if (!bmp_save_24(out_bmp, bgr.data(), width, height, error)) {
    std::fprintf(stderr, "[onevpl] decode: %s\n", error.c_str());
    return 1;
  }
  std::printf("[onevpl] decode ok: %ux%u -> %s (impl=%s)\n", width, height,
              out_bmp, impl.c_str());
  return 0;
}

int oic_onevpl_available(void) {
  return oic::jpeg::onevpl::vpl_available() ? 0 : 1;
}

int oic_onevpl_probe(char *out, int capacity) {
  const std::string report = oic::jpeg::onevpl::vpl_probe().describe();
  const int need = static_cast<int>(report.size());
  if (out != nullptr && capacity > 0) {
    const int copy = need < capacity - 1 ? need : capacity - 1;
    std::memcpy(out, report.data(), static_cast<size_t>(copy));
    out[copy] = '\0';
  }
  return need;
}

} // extern "C"
