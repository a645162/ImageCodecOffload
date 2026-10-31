// cpu_backend.cpp - CPU 后端的格式分派。
#include "cpu_backend.h"

#include "jpeg_io.h"
#include "metrics.h"
#include "png_io.h"
#include "tiff_io.h"

namespace image_codec {
namespace offload {
namespace cpu {

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
