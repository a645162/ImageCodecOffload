// image_codec_offload.cpp - 本库的 C API 实现。
//
// 这一层只做三件事：参数校验、ico_* 与内部 C++ 类型之间的转换、把一次调用的
// 后端选择结果（含回退原因）暂存到句柄上供 image_codec_offload_last_outcome 取。
// 任何格式逻辑都不允许出现在这里——它一旦开始长业务，就成了第二个 router。
#include "ImageCodecOffload/image_codec_offload.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "image_codec_types.h"
#include "router.h"

using image_codec::offload::BackendKind;
using image_codec::offload::EncodeParams;
using image_codec::offload::Error;
using image_codec::offload::Format;
using image_codec::offload::Image;
using image_codec::offload::Outcome;
using image_codec::offload::Router;
using image_codec::offload::SsimResult;
using image_codec::offload::Status;

struct ico_codec {
  Router router;

  // 最近一次调用的诊断信息。不是线程安全的，与整套 API 的约定一致。
  int last_backend = ICO_BACKEND_AUTO;
  std::string last_detail;
};

namespace {

ico_status mapStatus(Status status) {
  switch (status) {
    case Status::Ok:
      return ICO_OK;
    case Status::InvalidArgument:
      return ICO_ERR_INVALID_ARGUMENT;
    case Status::OutOfMemory:
      return ICO_ERR_OUT_OF_MEMORY;
    case Status::BackendUnavailable:
      return ICO_ERR_BACKEND_UNAVAILABLE;
    case Status::UnsupportedFormat:
      return ICO_ERR_UNSUPPORTED_FORMAT;
    case Status::DecodeFailed:
      return ICO_ERR_DECODE_FAILED;
    case Status::EncodeFailed:
      return ICO_ERR_ENCODE_FAILED;
    case Status::Io:
      return ICO_ERR_IO;
    case Status::Internal:
      return ICO_ERR_INTERNAL;
  }
  return ICO_ERR_INTERNAL;
}

Format toFormat(int format) {
  switch (format) {
    case ICO_FORMAT_JPEG:
      return Format::Jpeg;
    case ICO_FORMAT_TIFF:
      return Format::Tiff;
    case ICO_FORMAT_PNG:
      return Format::Png;
    default:
      return Format::Unknown;
  }
}

BackendKind toBackendKind(int backend) {
  switch (backend) {
    case ICO_BACKEND_CPU:
      return BackendKind::Cpu;
    case ICO_BACKEND_OPENCL:
      return BackendKind::OpenCl;
    case ICO_BACKEND_CUDA:
      return BackendKind::Cuda;
    case ICO_BACKEND_VULKAN:
      return BackendKind::Vulkan;
    default:
      return BackendKind::Cpu;
  }
}

EncodeParams toParams(const ico_encode_params* params) {
  EncodeParams out;  // 内部默认值
  if (params == nullptr) return out;

  if (params->jpeg_quality > 0) out.jpeg_quality = params->jpeg_quality;
  if (params->jpeg_subsampling != 0) {
    out.jpeg_subsampling = params->jpeg_subsampling;
  }

  if (params->tiff_compression != 0) {
    out.tiff_compression = params->tiff_compression;
  }
  if (params->tiff_rows_per_strip > 0) {
    out.tiff_rows_per_strip = params->tiff_rows_per_strip;
  }
  if (params->tiff_tile_size > 0) out.tiff_tile_size = params->tiff_tile_size;

  // PNG 的 -1 是「自适应」，与「未填(0)」不同；只有显式给了非 0 才覆盖。
  if (params->png_compression < 0) {
    out.png_compression = 6;
  } else if (params->png_compression > 0) {
    out.png_compression = params->png_compression;
  }
  if (params->png_filter != 0) out.png_filter = params->png_filter;

  return out;
}

// ico_image -> Image。stride 不为紧凑布局时逐行拷贝（不假设调用方给了 padding）。
bool importImage(const ico_image* src, Image* out) {
  if (src == nullptr || src->data == nullptr) return false;
  if (src->width == 0 || src->height == 0 || src->channels == 0) return false;

  const size_t tight = static_cast<size_t>(src->width) * src->channels;
  const size_t stride = src->stride == 0 ? tight : src->stride;
  if (stride < tight) return false;

  out->width = static_cast<int>(src->width);
  out->height = static_cast<int>(src->height);
  out->channels = static_cast<int>(src->channels);
  out->pixels.resize(tight * static_cast<size_t>(src->height));

  if (stride == tight) {
    std::memcpy(out->pixels.data(), src->data, out->pixels.size());
    return true;
  }

  for (size_t y = 0; y < static_cast<size_t>(src->height); ++y) {
    std::memcpy(out->pixels.data() + y * tight, src->data + y * stride, tight);
  }
  return true;
}

// Image -> ico_image（malloc，由调用方 free）。
bool exportImage(const Image& src, ico_image* out) {
  std::memset(out, 0, sizeof(*out));
  out->width = static_cast<uint32_t>(src.width);
  out->height = static_cast<uint32_t>(src.height);
  out->channels = static_cast<uint32_t>(src.channels);
  out->stride = src.rowBytes();
  out->data_size = src.pixels.size();
  out->data = static_cast<uint8_t*>(std::malloc(out->data_size));
  if (out->data == nullptr) return false;
  std::memcpy(out->data, src.pixels.data(), out->data_size);
  return true;
}

std::string joinNotes(const Outcome& outcome) {
  if (outcome.notes.empty()) return std::string();
  std::string joined;
  for (size_t i = 0; i < outcome.notes.size(); ++i) {
    if (i != 0) joined += "; ";
    joined += outcome.notes[i];
  }
  return joined;
}

void record(ico_codec* codec, const Outcome& outcome) {
  codec->last_backend = static_cast<int>(outcome.used);
  codec->last_detail = joinNotes(outcome);
}

}  // namespace

// ---- 生命周期 ----

const char* image_codec_offload_status_string(ico_status status) {
  switch (status) {
    case ICO_OK:
      return "ok";
    case ICO_ERR_INVALID_ARGUMENT:
      return "invalid-argument";
    case ICO_ERR_OUT_OF_MEMORY:
      return "out-of-memory";
    case ICO_ERR_BACKEND_UNAVAILABLE:
      return "backend-unavailable";
    case ICO_ERR_UNSUPPORTED_FORMAT:
      return "unsupported-format";
    case ICO_ERR_DECODE_FAILED:
      return "decode-failed";
    case ICO_ERR_ENCODE_FAILED:
      return "encode-failed";
    case ICO_ERR_IO:
      return "io";
    case ICO_ERR_INTERNAL:
      return "internal";
  }
  return "unknown";
}

ico_status image_codec_offload_create(ico_codec** out_codec,
                                      int preferred_backend) {
  if (out_codec == nullptr) return ICO_ERR_INVALID_ARGUMENT;
  *out_codec = nullptr;

  auto codec = std::make_unique<ico_codec>();

  if (preferred_backend != ICO_BACKEND_AUTO) {
    // 首选排第一，其余按默认顺序兜底（CPU 永远最后，由 setOrder 保证）。
    std::vector<BackendKind> order;
    order.push_back(toBackendKind(preferred_backend));
    for (BackendKind kind : Router::defaultOrder()) {
      order.push_back(kind);
    }
    codec->router.setOrder(std::move(order));
  }

  *out_codec = codec.release();
  return ICO_OK;
}

void image_codec_offload_destroy(ico_codec* codec) { delete codec; }

ico_status image_codec_offload_set_order(ico_codec* codec, const int* kinds,
                                         int count) {
  if (codec == nullptr) return ICO_ERR_INVALID_ARGUMENT;
  if (count < 0 || (count > 0 && kinds == nullptr)) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  std::vector<BackendKind> order;
  order.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    if (kinds[i] == ICO_BACKEND_AUTO) continue;
    order.push_back(toBackendKind(kinds[i]));
  }

  codec->router.setOrder(std::move(order));
  return ICO_OK;
}

ico_status image_codec_offload_probe_backends(ico_codec* codec,
                                              ico_backend_info* out,
                                              int capacity, int* count) {
  if (codec == nullptr || count == nullptr) return ICO_ERR_INVALID_ARGUMENT;
  if (capacity < 0 || (capacity > 0 && out == nullptr)) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  const auto probed = codec->router.probeBackends();
  *count = static_cast<int>(probed.size());

  for (size_t i = 0; i < probed.size() && i < static_cast<size_t>(capacity);
       ++i) {
    std::memset(&out[i], 0, sizeof(out[i]));
    out[i].backend = static_cast<int>(probed[i].first);
    out[i].ready = probed[i].second == "ready" ? 1 : 0;
    // detail 固定 256 字节：超长原因要截断，但必须仍以 '\0' 结尾。
    std::strncpy(out[i].detail, probed[i].second.c_str(),
                 sizeof(out[i].detail) - 1);
    out[i].detail[sizeof(out[i].detail) - 1] = '\0';
  }
  return ICO_OK;
}

ico_status image_codec_offload_last_outcome(ico_codec* codec, int* used_backend,
                                            char* detail,
                                            size_t detail_capacity) {
  if (codec == nullptr) return ICO_ERR_INVALID_ARGUMENT;
  if (used_backend != nullptr) *used_backend = codec->last_backend;
  if (detail != nullptr && detail_capacity > 0) {
    std::strncpy(detail, codec->last_detail.c_str(), detail_capacity - 1);
    detail[detail_capacity - 1] = '\0';
  }
  return ICO_OK;
}

// ---- 编解码 ----

ico_status image_codec_offload_probe(const void* data, size_t size,
                                     int* out_format) {
  if (out_format == nullptr) return ICO_ERR_INVALID_ARGUMENT;
  const Format format = image_codec::offload::probeFormat(
      static_cast<const uint8_t*>(data), size);
  switch (format) {
    case Format::Jpeg:
      *out_format = ICO_FORMAT_JPEG;
      break;
    case Format::Tiff:
      *out_format = ICO_FORMAT_TIFF;
      break;
    case Format::Png:
      *out_format = ICO_FORMAT_PNG;
      break;
    default:
      *out_format = ICO_FORMAT_UNKNOWN;
      break;
  }
  return ICO_OK;
}

ico_status image_codec_offload_decode(ico_codec* codec, int format,
                                      const void* in, size_t in_len,
                                      ico_image* out) {
  if (codec == nullptr || in == nullptr || in_len == 0 || out == nullptr) {
    return ICO_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));

  Format fmt = toFormat(format);
  if (fmt == Format::Unknown) {
    // 调用方说「不知道」时自己看魔数，而不是要求它先调 probe。
    fmt = image_codec::offload::probeFormat(
        static_cast<const uint8_t*>(in), in_len);
  }
  if (fmt == Format::Unknown) {
    return ICO_ERR_UNSUPPORTED_FORMAT;
  }

  Outcome outcome;
  const auto decoded = codec->router.decode(
      fmt, static_cast<const uint8_t*>(in), in_len, &outcome);
  record(codec, outcome);
  if (!decoded) return mapStatus(decoded.error().code);

  if (!exportImage(*decoded, out)) {
    return ICO_ERR_OUT_OF_MEMORY;
  }
  return ICO_OK;
}

ico_status image_codec_offload_encode(ico_codec* codec, int format,
                                      const ico_image* img,
                                      const ico_encode_params* params,
                                      void* out, size_t* out_len,
                                      size_t out_capacity) {
  if (codec == nullptr || img == nullptr || out_len == nullptr) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  const Format fmt = toFormat(format);
  if (fmt == Format::Unknown) return ICO_ERR_UNSUPPORTED_FORMAT;

  Image image;
  if (!importImage(img, &image)) return ICO_ERR_INVALID_ARGUMENT;

  Outcome outcome;
  const auto encoded =
      codec->router.encode(fmt, image, toParams(params), &outcome);
  record(codec, outcome);
  if (!encoded) return mapStatus(encoded.error().code);

  // 先查后写：缓冲不足时把所需大小写回，调用方据此扩容。
  if (out == nullptr || out_capacity < encoded->size()) {
    *out_len = encoded->size();
    return ICO_ERR_OUT_OF_MEMORY;
  }

  std::memcpy(out, encoded->data(), encoded->size());
  *out_len = encoded->size();
  return ICO_OK;
}

void image_codec_offload_image_free(ico_image* img) {
  if (img == nullptr) return;
  if (img->data != nullptr) std::free(img->data);
  std::memset(img, 0, sizeof(*img));
}

// ---- 指标 ----

ico_status image_codec_offload_metric_mse(ico_codec* codec, const ico_image* a,
                                          const ico_image* b,
                                          double* out_value) {
  if (codec == nullptr || out_value == nullptr) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  Image ia;
  Image ib;
  if (!importImage(a, &ia) || !importImage(b, &ib)) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  Outcome outcome;
  const auto value = codec->router.mse(ia, ib, &outcome);
  record(codec, outcome);
  if (!value) return mapStatus(value.error().code);

  *out_value = *value;
  return ICO_OK;
}

ico_status image_codec_offload_metric_psnr(ico_codec* codec, const ico_image* a,
                                           const ico_image* b,
                                           double* out_value) {
  if (codec == nullptr || out_value == nullptr) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  Image ia;
  Image ib;
  if (!importImage(a, &ia) || !importImage(b, &ib)) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  Outcome outcome;
  const auto value = codec->router.psnr(ia, ib, &outcome);
  record(codec, outcome);
  if (!value) return mapStatus(value.error().code);

  *out_value = *value;
  return ICO_OK;
}

ico_status image_codec_offload_metric_ssim(ico_codec* codec, const ico_image* a,
                                           const ico_image* b, ico_ssim* out) {
  if (codec == nullptr || out == nullptr) return ICO_ERR_INVALID_ARGUMENT;
  std::memset(out, 0, sizeof(*out));

  Image ia;
  Image ib;
  if (!importImage(a, &ia) || !importImage(b, &ib)) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  Outcome outcome;
  const auto value = codec->router.ssim(ia, ib, &outcome);
  record(codec, outcome);
  if (!value) return mapStatus(value.error().code);

  out->ssim = value->ssim;
  out->ms_ssim = value->ms_ssim;
  out->levels = value->levels;
  return ICO_OK;
}
