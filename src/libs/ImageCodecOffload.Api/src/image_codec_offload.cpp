/*
 * File: image_codec_offload.cpp
 * Brief: Image codec offload.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

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
#include "tiff_container.h"
#include "tiff_io.h"

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

  // 强制后端（create_forced）：true 时所有调用只走 forced_kind，绝不回退。
  // 声明为「是否强制 + 哪一个」，而不是把 router 的 order 改成单项——order 的
  // 语义包含兜底，而强制模式的语义恰好是「不要兜底」，两者不能共用一条通路。
  bool forced = false;
  BackendKind forced_kind = BackendKind::Cpu;

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
    case ICO_FORMAT_JPEG2000:
      return Format::Jpeg2000;
    case ICO_FORMAT_JPEG_XL:
      return Format::JpegXl;
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

  // 进阶参数一律用「0 / 负数 = 未指定」表达。C 调用方习惯把结构体 memset 成
  // 0，若把 0 当成「显式给的质量 0」就会被夹到质量 1——一张几乎全是色块的图，
  // 而调用方以为自己什么都没改。因此只有**正数**才算显式指定；这也与既有字段
  // （png_compression 0 → 默认 6、png_filter 0 → 自适应）的约定一致。
  if (params->jpeg_progressive != 0) out.jpeg_progressive = true;
  if (params->jpeg_optimize_huffman != 0) out.jpeg_optimize_huffman = true;
  if (params->jpeg_restart_interval > 0) {
    out.jpeg_restart_interval = params->jpeg_restart_interval;
  }
  if (params->jpeg_luma_quality > 0) {
    out.jpeg_luma_quality = params->jpeg_luma_quality;
  }
  if (params->jpeg_chroma_quality > 0) {
    out.jpeg_chroma_quality = params->jpeg_chroma_quality;
  }

  if (params->tiff_predictor > 0) out.tiff_predictor = params->tiff_predictor;
  if (params->tiff_x_dpi > 0) out.tiff_x_dpi = params->tiff_x_dpi;
  if (params->tiff_y_dpi > 0) out.tiff_y_dpi = params->tiff_y_dpi;
  if (params->tiff_resolution_unit > 0) {
    out.tiff_resolution_unit = params->tiff_resolution_unit;
  }
  if (params->tiff_jpeg_quality > 0) {
    out.tiff_jpeg_quality = params->tiff_jpeg_quality;
  }

  if (params->png_strategy > 0) out.png_strategy = params->png_strategy;

  // PNG 与上面几个字段不同：等级 0 与「不滤波」都是合法取值，所以这里用
  // -1 表示未指定，而不是 0——否则这两个值永远表达不出来（0 会被当成
  // 「没填」而落回默认的 6 / 自适应）。
  if (params->png_compression >= 0) {
    out.png_compression = params->png_compression;
  }
  if (params->png_filter >= 0) out.png_filter = params->png_filter;

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

ico_status image_codec_offload_create_forced(ico_codec** out_codec,
                                             int backend) {
  if (out_codec == nullptr) return ICO_ERR_INVALID_ARGUMENT;
  *out_codec = nullptr;
  if (backend == ICO_BACKEND_AUTO) return ICO_ERR_INVALID_ARGUMENT;

  auto codec = std::make_unique<ico_codec>();
  codec->forced = true;
  codec->forced_kind = toBackendKind(backend);
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

ico_status
image_codec_offload_encode_capabilities(ico_codec* codec,
                                        ico_encode_capabilities* out) {
  if (codec == nullptr || out == nullptr) return ICO_ERR_INVALID_ARGUMENT;

  const auto caps = codec->forced
                        ? codec->router.encodeCapabilitiesFor(codec->forced_kind)
                        : codec->router.encodeCapabilities();
  out->jpeg_progressive = caps.jpeg_progressive ? 1 : 0;
  out->jpeg_optimize_huffman = caps.jpeg_optimize_huffman ? 1 : 0;
  out->jpeg_restart_interval = caps.jpeg_restart_interval ? 1 : 0;
  out->jpeg_luma_chroma_quality = caps.jpeg_luma_chroma_quality ? 1 : 0;
  out->tiff_predictor = caps.tiff_predictor ? 1 : 0;
  out->tiff_resolution = caps.tiff_resolution ? 1 : 0;
  out->tiff_packbits = caps.tiff_packbits ? 1 : 0;
  out->tiff_jpeg_compression = caps.tiff_jpeg_compression ? 1 : 0;
  out->png_strategy = caps.png_strategy ? 1 : 0;
  out->tiff_strip_sharding = caps.tiff_strip_sharding ? 1 : 0;
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
  const auto decoded =
      codec->forced
          ? codec->router.decodeWith(codec->forced_kind, fmt,
                                     static_cast<const uint8_t*>(in), in_len)
          : codec->router.decode(fmt, static_cast<const uint8_t*>(in), in_len,
                                 &outcome);
  if (codec->forced) outcome.used = codec->forced_kind;
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
  const auto encoded = codec->forced
                           ? codec->router.encodeWith(codec->forced_kind, fmt,
                                                      image, toParams(params))
                           : codec->router.encode(fmt, image,
                                                  toParams(params), &outcome);
  if (codec->forced) outcome.used = codec->forced_kind;
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

ico_status image_codec_offload_tiff_strip_encode(
    ico_codec* codec, const ico_image* img, int first_row, int rows,
    const ico_encode_params* params, void* out, size_t* out_len,
    size_t out_capacity) {
  if (codec == nullptr || img == nullptr || out_len == nullptr) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  Image image;
  if (!importImage(img, &image)) return ICO_ERR_INVALID_ARGUMENT;

  Outcome outcome;
  const auto encoded =
      codec->forced
          ? codec->router.encodeTiffStripWith(codec->forced_kind, image,
                                              first_row, rows,
                                              toParams(params))
          : codec->router.encodeTiffStrip(image, first_row, rows,
                                          toParams(params), &outcome);
  if (codec->forced) outcome.used = codec->forced_kind;
  record(codec, outcome);
  if (!encoded) return mapStatus(encoded.error().code);

  if (out == nullptr || out_capacity < encoded->size()) {
    *out_len = encoded->size();
    return ICO_ERR_OUT_OF_MEMORY;
  }
  std::memcpy(out, encoded->data(), encoded->size());
  *out_len = encoded->size();
  return ICO_OK;
}

ico_status image_codec_offload_tiff_assemble(
    const ico_tiff_assembly* assembly, const void* const* strips,
    const size_t* strip_sizes, int strip_count, void* out, size_t* out_len,
    size_t out_capacity) {
  if (assembly == nullptr || out_len == nullptr || strip_count <= 0 ||
      strips == nullptr || strip_sizes == nullptr) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  // 片流按顺序交给拼装器；这里只做「条数/尺寸」这类结构校验，压缩参数的解释
  // 交给 layoutFromParams（与整图编码同一套映射，避免两处各写一份缺省值）。
  std::vector<std::vector<uint8_t>> payloads;
  payloads.reserve(static_cast<size_t>(strip_count));
  for (int index = 0; index < strip_count; ++index) {
    if (strips[index] == nullptr || strip_sizes[index] == 0) {
      return ICO_ERR_INVALID_ARGUMENT;
    }
    const auto* begin = static_cast<const uint8_t*>(strips[index]);
    payloads.emplace_back(begin, begin + strip_sizes[index]);
  }

  Image shape;
  shape.width = assembly->width;
  shape.height = assembly->height;
  shape.channels = assembly->channels;
  if (shape.width <= 0 || shape.height <= 0 || shape.channels < 1 ||
      shape.channels > 4) {
    return ICO_ERR_INVALID_ARGUMENT;
  }

  const auto layout = image_codec::offload::cpu::layoutFromParams(
      shape, toParams(&assembly->params), assembly->rows_per_strip);
  const auto assembled =
      image_codec::offload::cpu::assembleClassicTiff(layout, payloads);
  if (!assembled) return mapStatus(assembled.error().code);

  if (out == nullptr || out_capacity < assembled->size()) {
    *out_len = assembled->size();
    return ICO_ERR_OUT_OF_MEMORY;
  }
  std::memcpy(out, assembled->data(), assembled->size());
  *out_len = assembled->size();
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
  const auto value =
      codec->forced
          ? codec->router.mseWith(codec->forced_kind, ia, ib)
          : codec->router.mse(ia, ib, &outcome);
  if (codec->forced) outcome.used = codec->forced_kind;
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
  const auto value =
      codec->forced
          ? codec->router.psnrWith(codec->forced_kind, ia, ib)
          : codec->router.psnr(ia, ib, &outcome);
  if (codec->forced) outcome.used = codec->forced_kind;
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
  const auto value =
      codec->forced
          ? codec->router.ssimWith(codec->forced_kind, ia, ib)
          : codec->router.ssim(ia, ib, &outcome);
  if (codec->forced) outcome.used = codec->forced_kind;
  record(codec, outcome);
  if (!value) return mapStatus(value.error().code);

  out->ssim = value->ssim;
  out->ms_ssim = value->ms_ssim;
  out->levels = value->levels;
  return ICO_OK;
}
