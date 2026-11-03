/*
 * File: tiff_test.cpp
 * Brief: Tiff test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/tiff_test.cpp - CLI 测试入口实现（BMP <-> TIFF）。
#include "tiff_test.h"

#include <cstdio>
#include <string>
#include <vector>

#include "bmp_io.h"
#include "lzw_codec.h"
#include "tiff_container.h"

int image_codec_tiff_info(const char *path) {
  if (path == nullptr) return -1;
  image_codec::tiff::TiffReader r;
  if (!r.openFile(path)) {
    std::fprintf(stderr, "image_codec_tiff_info: 无法解析 TIFF 文件: %s\n",
                 path);
    return -2;
  }
  std::printf("TIFF info: %s\n", path);
  r.printInfo(stdout);
  return 0;
}

int image_codec_tiff_decode(const char *in, const char *out_bmp, int backend) {
  (void)backend; // 解码统一走 CPU；参数保留以便 CLI 语义一致。
  if (in == nullptr || out_bmp == nullptr) return -1;

  int w = 0, h = 0, channels = 0;
  std::vector<uint8_t> pixels;
  const int rc =
      image_codec::tiff::tiffDecodeFromFile(in, w, h, channels, pixels);
  if (rc != 0) {
    std::fprintf(stderr, "image_codec_tiff_decode: TIFF 解码失败 (%d)\n", rc);
    return rc;
  }

  // 通道保真：spp==1 的 TIFF 就写 8-bit 灰度 BMP，不做 RGB 升维。
  image_codec::ImageBuffer img;
  if (!img.allocate(w, h, channels)) return -3;
  img.data = std::move(pixels);

  std::string err;
  const int rc2 = image_codec::bmp_write(out_bmp, img, &err);
  if (rc2 != 0) {
    std::fprintf(stderr, "image_codec_tiff_decode: BMP 写出失败 (%d): %s\n",
                 rc2, err.c_str());
    return rc2;
  }
  return 0;
}

int image_codec_tiff_encode(const char *in_bmp, const char *out, int backend,
                            int compression, int rows_per_strip) {
  if (in_bmp == nullptr || out == nullptr) return -1;
  if (compression != 1 && compression != 5) {
    std::fprintf(stderr,
                 "image_codec_tiff_encode: 不支持的压缩 %d（1=none, 5=LZW）\n",
                 compression);
    return -2;
  }

  image_codec::ImageBuffer img;
  std::string err;
  const int rc = image_codec::bmp_read(in_bmp, &img, &err);
  if (rc != 0) {
    std::fprintf(stderr, "image_codec_tiff_encode: BMP 读取失败 (%d): %s\n", rc,
                 err.c_str());
    return rc;
  }

  const int rc2 = image_codec::tiff::tiffEncodeToFile(
      out, img.width, img.height, img.channels, img.data.data(), compression,
      rows_per_strip, backend);
  if (rc2 != 0) {
    std::fprintf(stderr, "image_codec_tiff_encode: TIFF 编码失败 (%d): %s\n",
                 rc2, out);
    return rc2;
  }
  return 0;
}
