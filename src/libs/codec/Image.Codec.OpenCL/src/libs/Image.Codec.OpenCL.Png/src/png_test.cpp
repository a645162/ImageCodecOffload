/*
 * File: png_test.cpp
 * Brief: Png test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// png_test.cpp - PNG 模块文件入口实现（BMP <-> PNG）。
#include "png_test.h"

#include <cstdio>
#include <string>
#include <vector>

#include "bmp_io.h"
#include "png_codec.h"
#include "png_container.h"

namespace {

bool read_file(const char *path, std::vector<uint8_t> *out) {
  FILE *f = std::fopen(path, "rb");
  if (f == nullptr) return false;
  uint8_t buf[65536];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out->insert(out->end(), buf, buf + n);
  }
  std::fclose(f);
  return true;
}

bool write_file(const char *path, const std::vector<uint8_t> &data) {
  FILE *f = std::fopen(path, "wb");
  if (f == nullptr) return false;
  const size_t n = std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
  return n == data.size();
}

} // namespace

extern "C" {

int image_codec_png_info(const char *path) {
  if (path == nullptr) return -1;
  std::vector<uint8_t> file;
  if (!read_file(path, &file)) {
    std::fprintf(stderr, "image_codec_png_info: cannot read %s\n", path);
    return -1;
  }
  if (!image_codec::ocl::png::hasPngSignature(file.data(), file.size())) {
    std::fprintf(stderr, "image_codec_png_info: not a PNG file: %s\n", path);
    return -2;
  }
  const int channels =
      image_codec::ocl::png::probeChannels(file.data(), file.size());
  if (channels < 0) {
    std::fprintf(stderr, "image_codec_png_info: unsupported PNG: %s\n", path);
    return -3;
  }
  std::printf("PNG: %s, %d channel(s)\n", path, channels);
  return 0;
}

int image_codec_png_decode(const char *in, const char *out_bmp, int backend) {
  (void)backend; // 解码的 inflate/unfilter 在 CPU，参数保留以与其它模块一致。
  if (in == nullptr || out_bmp == nullptr) return -1;

  std::vector<uint8_t> file;
  if (!read_file(in, &file)) {
    std::fprintf(stderr, "image_codec_png_decode: cannot read %s\n", in);
    return -1;
  }

  image_codec::ImageBuffer img;
  std::string err;
  if (!image_codec::ocl::png::decodePng(file.data(), file.size(), &img, &err)) {
    std::fprintf(stderr, "image_codec_png_decode: %s\n", err.c_str());
    return -2;
  }

  // 灰度（1 通道）写 8-bit BMP；3/4 通道写 24-bit RGB。命名通道保真：
  // PNG 的 4 通道数据在这里按 RGB 落盘（Alpha 不进商品图像数据，见 AGENTS 约定）。
  if (img.channels == 2 || img.channels == 4) {
    // gray+alpha / RGBA：丢掉 alpha，保留颜色通道。
    const int keep = (img.channels == 2) ? 1 : 3;
    image_codec::ImageBuffer compact;
    if (!compact.allocate(img.width, img.height, keep)) return -3;
    const int src_bpp = img.channels;
    for (int i = 0; i < img.width * img.height; ++i) {
      for (int c = 0; c < keep; ++c) {
        compact.data[static_cast<size_t>(i) * keep + c] =
            img.data[static_cast<size_t>(i) * src_bpp + c];
      }
    }
    img = std::move(compact);
  }
  if (img.channels != 1 && img.channels != 3) {
    std::fprintf(stderr,
                 "image_codec_png_decode: unsupported channel count %d\n",
                 img.channels);
    return -3;
  }

  if (image_codec::bmp_write(out_bmp, img, &err) != 0) {
    std::fprintf(stderr, "image_codec_png_decode: %s\n", err.c_str());
    return -4;
  }
  return 0;
}

int image_codec_png_encode(const char *in_bmp, const char *out, int backend,
                           int compression, int filter) {
  if (in_bmp == nullptr || out == nullptr) return -1;

  image_codec::ImageBuffer img;
  std::string err;
  if (image_codec::bmp_read(in_bmp, &img, &err) != 0) {
    std::fprintf(stderr, "image_codec_png_encode: %s\n", err.c_str());
    return -2;
  }

  image_codec::ocl::png::PngEncodeOptions options;
  options.use_gpu = (backend != 2);
  options.filter = filter;
  options.compression_level = compression < 0 ? 6 : compression;

  std::vector<uint8_t> encoded;
  bool gpu_used = false;
  if (!image_codec::ocl::png::encodePng(img, options, &encoded, &gpu_used,
                                        &err)) {
    std::fprintf(stderr, "image_codec_png_encode: %s\n", err.c_str());
    return -3;
  }
  if (!write_file(out, encoded)) {
    std::fprintf(stderr, "image_codec_png_encode: cannot write %s\n", out);
    return -4;
  }
  return 0;
}

} // extern "C"
