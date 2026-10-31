/*
 * File: image_codec_types.cpp
 * Brief: Image codec types.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// image_codec_types.cpp - 公共类型的字符串化与格式嗅探。
#include "image_codec_types.h"

namespace image_codec {
namespace offload {

const char* statusName(Status status) {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::InvalidArgument:
      return "invalid-argument";
    case Status::OutOfMemory:
      return "out-of-memory";
    case Status::BackendUnavailable:
      return "backend-unavailable";
    case Status::UnsupportedFormat:
      return "unsupported-format";
    case Status::DecodeFailed:
      return "decode-failed";
    case Status::EncodeFailed:
      return "encode-failed";
    case Status::Io:
      return "io";
    case Status::Internal:
      return "internal";
  }
  return "unknown";
}

const char* formatName(Format format) {
  switch (format) {
    case Format::Jpeg:
      return "jpeg";
    case Format::Tiff:
      return "tiff";
    case Format::Png:
      return "png";
    case Format::Unknown:
      break;
  }
  return "unknown";
}

const char* backendName(BackendKind kind) {
  switch (kind) {
    case BackendKind::Cpu:
      return "cpu";
    case BackendKind::OpenCl:
      return "opencl";
    case BackendKind::Cuda:
      return "cuda";
    case BackendKind::Vulkan:
      return "vulkan";
  }
  return "unknown";
}

Format probeFormat(const uint8_t* data, size_t size) {
  if (data == nullptr) return Format::Unknown;

  // PNG: 89 50 4E 47 0D 0A 1A 0A
  static constexpr uint8_t kPng[8] = {0x89, 0x50, 0x4E, 0x47,
                                      0x0D, 0x0A, 0x1A, 0x0A};
  if (size >= 8) {
    bool png = true;
    for (size_t i = 0; i < 8; ++i) {
      if (data[i] != kPng[i]) {
        png = false;
        break;
      }
    }
    if (png) return Format::Png;
  }

  // JPEG: FF D8 FF
  if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
    return Format::Jpeg;
  }

  // TIFF: little-endian "II*\0" / big-endian "MM\0*"
  if (size >= 4) {
    const bool le = data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A &&
                    data[3] == 0x00;
    const bool be = data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 &&
                    data[3] == 0x2A;
    if (le || be) return Format::Tiff;
  }

  return Format::Unknown;
}

}  // namespace offload
}  // namespace image_codec
