/*
 * File: image_codec_types.cpp
 * Brief: Image codec types.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// image_codec_types.cpp - 公共类型的字符串化与格式嗅探。
#include "image_codec_types.h"

#include <algorithm>
#include <cctype>
#include <limits>

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
    case Format::Jpeg2000:
      return "jpeg2000";
    case Format::JpegXl:
      return "jpeg-xl";
    case Format::Unknown:
      break;
  }
  return "unknown";
}

const std::vector<FormatDescriptor> &formatDescriptors() {
  constexpr uint64_t kUint32Max = std::numeric_limits<uint32_t>::max();
  // The public Image model stores geometry as signed int.  Do not advertise a
  // codec/container coordinate range which this API cannot represent.
  constexpr uint64_t kImageDimensionMax = std::numeric_limits<int>::max();
  static const std::vector<FormatDescriptor> descriptors = {
      {Format::Jpeg, "JPEG", "传统 JPEG（ISO/IEC 10918-1）。",
       {".jpg", ".jpeg", ".jpe"}, {"image/jpeg"}, 65535, 65535, 65535,
       65535, true, true, true, false, true},
      {Format::Tiff, "TIFF", "支持条带和瓦片布局的 TIFF 容器。",
       {".tif", ".tiff"}, {"image/tiff"}, 0, 0, 0, 0, true, true, true,
       true, true},
      {Format::Png, "PNG", "无损 PNG 图像。", {".png"}, {"image/png"}, 0,
       0, 0, 0, true, true, true, false, true},
      {Format::Jpeg2000, "JPEG 2000", "基于小波变换的 JPEG 2000。",
       {".jp2", ".j2k", ".jpf", ".jpx"}, {"image/jp2", "image/jpx"},
       kUint32Max, kUint32Max, kImageDimensionMax, kImageDimensionMax, true,
       true, true, true, true},
      {Format::JpegXl, "JPEG XL", "新一代 JPEG XL 图像格式。", {".jxl"},
       {"image/jxl"}, kUint32Max, kUint32Max, kImageDimensionMax,
       kImageDimensionMax, true, true, true, true, false},
  };
  return descriptors;
}

const FormatDescriptor *formatDescriptor(Format format) {
  const auto &descriptors = formatDescriptors();
  const auto it = std::find_if(descriptors.begin(), descriptors.end(),
                               [format](const FormatDescriptor &descriptor) {
                                 return descriptor.format == format;
                               });
  return it == descriptors.end() ? nullptr : &*it;
}

Format formatFromExtension(std::string_view extension) {
  std::string normalized;
  normalized.reserve(extension.size() + 1);
  for (const char ch : extension) {
    normalized.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  if (!normalized.empty() && normalized.front() != '.') {
    normalized.insert(normalized.begin(), '.');
  }
  for (const auto &descriptor : formatDescriptors()) {
    for (const auto candidate : descriptor.extensions) {
      if (normalized == candidate) return descriptor.format;
    }
  }
  return Format::Unknown;
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

const char* executionModeName(ExecutionMode mode) {
  switch (mode) {
    case ExecutionMode::Unavailable:
      return "unavailable";
    case ExecutionMode::Cpu:
      return "cpu";
    case ExecutionMode::HybridCpuGpu:
      return "hybrid-cpu-gpu";
    case ExecutionMode::NativeGpu:
      return "native-gpu";
  }
  return "unavailable";
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

  // JPEG 2000 JP2 box or raw codestream.
  if (size >= 12 && data[4] == 'j' && data[5] == 'P' && data[6] == ' ' &&
      data[7] == ' ' && data[8] == 0x0D && data[9] == 0x0A &&
      data[10] == 0x87 && data[11] == 0x0A) {
    return Format::Jpeg2000;
  }
  if (size >= 4 && data[0] == 0xFF && data[1] == 0x4F && data[2] == 0xFF &&
      data[3] == 0x51) {
    return Format::Jpeg2000;
  }

  // JPEG XL container or naked codestream.
  if (size >= 12 && data[4] == 'J' && data[5] == 'X' && data[6] == 'L' &&
      data[7] == ' ' && data[8] == 0x0D && data[9] == 0x0A &&
      data[10] == 0x87 && data[11] == 0x0A) {
    return Format::JpegXl;
  }
  if (size >= 2 && data[0] == 0xFF && data[1] == 0x0A) return Format::JpegXl;

  return Format::Unknown;
}

}  // namespace offload
}  // namespace image_codec
