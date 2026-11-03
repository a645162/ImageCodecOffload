/*
 * File: selftest_support.cpp
 * Brief: Selftest support.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// selftest_support.cpp - 自测参考实现（libjpeg / libtiff，按构建开关裁剪）。
#include "selftest_support.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(OIC_HAVE_LIBJPEG)
#include <jpeglib.h>

#include <cstdio>
#include <cstdlib>
#endif

#if defined(OIC_HAVE_LIBTIFF)
#include <tiffio.h>
#endif

namespace oic {
namespace selftest {

bool have_jpeg_encoder() {
#if defined(OIC_HAVE_LIBJPEG)
  return true;
#else
  return false;
#endif
}

bool have_tiff_decoder() {
#if defined(OIC_HAVE_LIBTIFF)
  return true;
#else
  return false;
#endif
}

bool encode_jpeg_reference(const uint8_t *rgb, uint32_t width, uint32_t rows,
                           uint32_t quality, std::vector<uint8_t> &jpeg,
                           std::string &error) {
  jpeg.clear();
#if defined(OIC_HAVE_LIBJPEG)
  if (rgb == nullptr || width == 0 || rows == 0) {
    error = "参考 JPEG 编码：输入为空";
    return false;
  }
  jpeg_compress_struct cinfo{};
  jpeg_error_mgr jerr{};
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);

  unsigned char *buffer = nullptr;
  unsigned long buffer_size = 0;
  jpeg_mem_dest(&cinfo, &buffer, &buffer_size);

  cinfo.image_width = width;
  cinfo.image_height = rows;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, static_cast<int>(quality), TRUE);
  // 4:2:0：与 Intel 硬件 JPEG 的默认采样保持一致，TIFF 的
  // YCbCrSubSampling 就是从 SOF0 读回来的。
  cinfo.comp_info[0].h_samp_factor = 2;
  cinfo.comp_info[0].v_samp_factor = 2;
  cinfo.comp_info[1].h_samp_factor = 1;
  cinfo.comp_info[1].v_samp_factor = 1;
  cinfo.comp_info[2].h_samp_factor = 1;
  cinfo.comp_info[2].v_samp_factor = 1;

  jpeg_start_compress(&cinfo, TRUE);
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(
        rgb + static_cast<size_t>(cinfo.next_scanline) * width * 3);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg.assign(buffer, buffer + buffer_size);
  std::free(buffer);
  jpeg_destroy_compress(&cinfo);
  return true;
#else
  (void)rgb;
  (void)width;
  (void)rows;
  (void)quality;
  error = "构建时没有找到 libjpeg";
  return false;
#endif
}

#if defined(OIC_HAVE_LIBTIFF)
namespace {

struct MemoryReader {
  const uint8_t *data = nullptr;
  size_t size = 0;
  size_t pos = 0;
};

tmsize_t read_callback(thandle_t handle, void *buffer, tmsize_t count) {
  auto *reader = static_cast<MemoryReader *>(handle);
  if (reader == nullptr || buffer == nullptr || count <= 0) {
    return 0;
  }
  const size_t want = static_cast<size_t>(count);
  const size_t left =
      reader->pos < reader->size ? reader->size - reader->pos : 0;
  const size_t take = want < left ? want : left;
  std::memcpy(buffer, reader->data + reader->pos, take);
  reader->pos += take;
  return static_cast<tmsize_t>(take);
}

toff_t seek_callback(thandle_t handle, toff_t offset, int whence) {
  auto *reader = static_cast<MemoryReader *>(handle);
  if (reader == nullptr) {
    return 0;
  }
  size_t base = 0;
  if (whence == SEEK_CUR) {
    base = reader->pos;
  } else if (whence == SEEK_END) {
    base = reader->size;
  }
  const size_t next = base + static_cast<size_t>(offset);
  reader->pos = next > reader->size ? reader->size : next;
  return static_cast<toff_t>(reader->pos);
}

toff_t size_callback(thandle_t handle) {
  auto *reader = static_cast<MemoryReader *>(handle);
  return reader != nullptr ? static_cast<toff_t>(reader->size) : 0;
}

int map_callback(thandle_t, void **, toff_t *) { return 0; }
void unmap_callback(thandle_t, void *, toff_t) {}
// libtiff 要求读写/关闭回调全部非空，哪怕这条路径只做解码。
tmsize_t write_ignored(thandle_t, void *, tmsize_t) { return -1; }
int close_callback(thandle_t) { return 0; }

} // namespace
#endif

bool decode_tiff_reference(const std::vector<uint8_t> &tiff, uint32_t &width,
                           uint32_t &height, std::vector<uint8_t> &rgb,
                           std::string &error) {
  rgb.clear();
#if defined(OIC_HAVE_LIBTIFF)
  MemoryReader reader;
  reader.data = tiff.data();
  reader.size = tiff.size();

  TIFF *file = TIFFClientOpen("memory", "r", &reader, read_callback,
                              write_ignored, seek_callback, close_callback,
                              size_callback, map_callback, unmap_callback);
  if (file == nullptr) {
    error = "libtiff 无法打开生成的 TIFF";
    return false;
  }
  uint32_t w = 0;
  uint32_t h = 0;
  uint16_t compression = 0;
  TIFFGetField(file, TIFFTAG_IMAGEWIDTH, &w);
  TIFFGetField(file, TIFFTAG_IMAGELENGTH, &h);
  TIFFGetField(file, TIFFTAG_COMPRESSION, &compression);
  // 让 libtiff 把 JPEG 的 YCbCr 转成 RGB（否则拿到的是原始 YCbCr 分量）。
  // 只有 JPEG 压缩才认这个伪标签，对未压缩文件设置会打印未知标签告警。
  if (compression == COMPRESSION_JPEG) {
    if (TIFFSetField(file, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB) != 1) {
      TIFFClose(file);
      error = "libtiff 拒绝 JPEGCOLORMODE_RGB";
      return false;
    }
  }
  if (w == 0 || h == 0) {
    TIFFClose(file);
    error = "libtiff 读到的宽高为 0";
    return false;
  }
  rgb.assign(static_cast<size_t>(w) * h * 3, 0);
  std::vector<uint8_t> strip(static_cast<size_t>(w) * 3 *
                             static_cast<size_t>(h));
  tstrip_t strips = TIFFNumberOfStrips(file);
  uint32_t rows_per_strip = 0;
  TIFFGetFieldDefaulted(file, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
  if (rows_per_strip == 0) {
    rows_per_strip = h;
  }
  for (tstrip_t index = 0; index < strips; ++index) {
    const uint32_t y = static_cast<uint32_t>(index) * rows_per_strip;
    if (y >= h) {
      break;
    }
    const uint32_t rows = std::min(rows_per_strip, h - y);
    const tmsize_t want = static_cast<tmsize_t>(rows) * w * 3;
    const tmsize_t got = TIFFReadEncodedStrip(file, index, strip.data(), want);
    if (got < 0) {
      TIFFClose(file);
      error = "libtiff 解码 strip 失败";
      return false;
    }
    std::memcpy(rgb.data() + static_cast<size_t>(y) * w * 3, strip.data(),
                static_cast<size_t>(want));
  }
  TIFFClose(file);
  width = w;
  height = h;
  return true;
#else
  (void)tiff;
  (void)width;
  (void)height;
  error = "构建时没有找到 libtiff";
  return false;
#endif
}

std::vector<uint8_t> make_test_image(uint32_t width, uint32_t height) {
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3, 0);
  const size_t pixels = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < pixels; ++i) {
    const uint32_t x = static_cast<uint32_t>(i % width);
    const uint32_t y = static_cast<uint32_t>(i / width);
    uint8_t &r = rgb[i * 3 + 0];
    uint8_t &g = rgb[i * 3 + 1];
    uint8_t &b = rgb[i * 3 + 2];
    r = static_cast<uint8_t>((x * 255) / (width > 1 ? width - 1 : 1));
    g = static_cast<uint8_t>((y * 255) / (height > 1 ? height - 1 : 1));
    b = static_cast<uint8_t>(((x / 16 + y / 16) & 1) != 0 ? 32 : 224);
  }
  return rgb;
}

double psnr_rgb(const uint8_t *a, const uint8_t *b, size_t bytes) {
  if (a == nullptr || b == nullptr || bytes == 0) {
    return 0.0;
  }
  double sum = 0.0;
  for (size_t i = 0; i < bytes; ++i) {
    const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += diff * diff;
  }
  const double mse = sum / static_cast<double>(bytes);
  if (mse <= 0.0) {
    return 999.0;
  }
  return 10.0 * std::log10((255.0 * 255.0) / mse);
}

} // namespace selftest
} // namespace oic
