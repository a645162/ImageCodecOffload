/*
 * File: cpu_backend.cpp
 * Brief: Cpu backend.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// cpu_backend.cpp - CPU 后端的格式分派。
#include "cpu_backend.h"

#include <tiffio.h>

#include "jpeg_io.h"
#include "metrics.h"
#include "png_io.h"
#include "tiff_io.h"

namespace image_codec {
namespace offload {
namespace cpu {

EncodeCapabilities CpuBackend::encodeCapabilities() const {
  EncodeCapabilities cap;
  cap.jpeg_progressive = true;
  cap.jpeg_optimize_huffman = true;
  cap.jpeg_restart_interval = true;
  cap.jpeg_luma_chroma_quality = true;
  cap.tiff_predictor = true;
  cap.tiff_resolution = true;
  cap.tiff_packbits = true;
  // libtiff 是否真的编入了 JPEG 编解码是**构建期**决定的：vcpkg 的 tiff
  // 端口可以不带 jpeg 特性。此时 TIFF 的 JPEG 压缩会「参数合法、写到一半
  // 失败」，必须在能力表里如实报出来，让上层改参数而不是换后端硬试。
  cap.tiff_jpeg_compression = TIFFIsCODECConfigured(COMPRESSION_JPEG) != 0;
  cap.png_strategy = true;
  // 分片：容器由 cpu::assembleClassicTiff() 自己写，条数/片高由调用方决定。
  // 压缩方式覆盖 NONE / LZW / Deflate / PackBits（JPEG-in-TIFF 与 tiled 不做）。
  cap.tiff_strip_sharding = true;
  return cap;
}

Result<std::vector<uint8_t>>
CpuBackend::encodeTiffStrip(const Image& image, const int first_row,
                            const int rows, const EncodeParams& params) {
  // 必须写完整限定：不限定会被解析成这个成员函数自己（无限递归）。
  return cpu::encodeTiffStrip(image, first_row, rows, params);
}

Result<std::vector<uint8_t>> CpuBackend::encode(Format format,
                                                const Image& image,
                                                const EncodeParams& params) {
  switch (format) {
    case Format::Jpeg:
      return encodeJpeg(image, params);
    case Format::Tiff:
      return encodeTiff(image, params);
    case Format::Png:
      return encodePng(image, params);
    case Format::Unknown:
      break;
  }
  return fail<std::vector<uint8_t>>(Status::UnsupportedFormat,
                                    "cpu: unsupported format");
}

Result<Image> CpuBackend::decode(Format format, const uint8_t* data,
                                 size_t size) {
  switch (format) {
    case Format::Jpeg:
      return decodeJpeg(data, size);
    case Format::Tiff:
      return decodeTiff(data, size);
    case Format::Png:
      return decodePng(data, size);
    case Format::Unknown:
      break;
  }
  return fail<Image>(Status::UnsupportedFormat, "cpu: unsupported format");
}

Result<double> CpuBackend::mse(const Image& a, const Image& b) {
  return metrics::mse(a, b);
}

Result<double> CpuBackend::psnr(const Image& a, const Image& b) {
  return metrics::psnr(a, b);
}

Result<SsimResult> CpuBackend::ssim(const Image& a, const Image& b) {
  return metrics::ssim(a, b);
}

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
