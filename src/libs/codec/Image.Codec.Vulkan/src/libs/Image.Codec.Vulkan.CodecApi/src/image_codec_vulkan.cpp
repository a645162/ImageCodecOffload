// File      : image_codec_vulkan.cpp
// Brief     : Vulkan 编解码后端对外 C API 实现（TIFF / PNG）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 本层只做三件事：参数校验、格式分派、C 结构体 <-> ImageBuffer 转换。
// 具体实现全部在各自模块里，避免这里长成第二个 codec。
//
// 句柄里刻意**只存设置**（device_index / memory_limit / use_gpu / 设备快照），
// 不长期持有 VkDevice：PNG 滤波、TIFF 预测器、指标各自按需建设备，只有它们
// 知道本次需要多大的缓冲。于是 get_memory_info 的 used/peak 如实报 0，
// 而不是编一个「看起来有记账」的数字（见头文件里的说明）。
#include "Image/Codec/Vulkan/image_codec_vulkan.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <vector>

#include "image_buffer.h"
#include "jpeg_codec.h"
#include "metrics.h"
#include "png_codec.h"
#include "png_container.h"
#include "tiff_codec.h"
#include "vk_buffer.h"
#include "vk_codec_device.h"

using image_codec::vulkan::Device;
using image_codec::vulkan::DeviceCaps;
using image_codec::vulkan::ImageBuffer;
using image_codec::vulkan::MemoryInfo;
using image_codec::vulkan::Buffer;

struct image_codec_vulkan_codec {
  int device_index = -1;
  bool use_gpu = true;
  size_t memory_limit = 0;

  // 创建时抓一次设备快照：能力表与显存查询都以它为准，避免「句柄建好了，
  // 但每次问设备名都要再建一次 VkDevice」。
  DeviceCaps caps;
  MemoryInfo memory;

  // 能力/降级说明（例如「JPEG 尚未实现」）。
  std::string note;
};

namespace {

// 可移植的 strdup（Windows 没有 POSIX strdup，且这里必须用 malloc 系列，
// 才能与 image_codec_vulkan_device_list_free 的 free 配对）。
char *dupString(const char *s) {
  const size_t n = std::strlen(s);
  auto *out = static_cast<char *>(std::malloc(n + 1));
  if (out == nullptr) return nullptr;
  std::memcpy(out, s, n + 1);
  return out;
}

std::string toLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return text;
}

// 失败原因 -> 状态码。
//
// 为什么要分类：C API 只回状态码，而「这个失败该不该让上层换一个后端」取决于
// 原因。规则与 CUDA 后端一致：
//   * 显存预算 / 设备分配失败 → OutOfMemory（CPU 后端用内存就能顶上）；
//   * 格式/子格式不支持 → UnsupportedFormat（别的后端可能能读）；
//   * 设备本身不可用（没有设备 / 驱动不支持 fp64）→ BackendUnavailable；
//     注意 fp64 只在指标调用里才是硬性要求，编解码在没有 fp64 的核显上照常跑。
//   * 其余 → DecodeFailed/EncodeFailed：真正的失败，不允许被别的后端悄悄救回来。
image_codec_vulkan_status classifyFailure(const std::string &err,
                                          bool encoding) {
  const std::string lower = toLower(err);
  const auto has = [&lower](const char *needle) {
    return lower.find(needle) != std::string::npos;
  };

  if (has("allocation failed") || has("out of memory") ||
      has("memory budget") || has("budget exceeded") ||
      has("out of device memory")) {
    return IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY;
  }
  if (has("does not support shaderfloat64") || has("no vulkan") ||
      has("no vulkan physical device") || has("vkcreateinstance") ||
      has("vkcreatedevice")) {
    return IMAGE_CODEC_VULKAN_ERR_BACKEND_UNAVAILABLE;
  }
  if (has("not support") || has("unsupported") || has("only 8-bit") ||
      has("palette")) {
    return IMAGE_CODEC_VULKAN_ERR_UNSUPPORTED_FORMAT;
  }
  return encoding ? IMAGE_CODEC_VULKAN_ERR_ENCODE_FAILED
                  : IMAGE_CODEC_VULKAN_ERR_DECODE_FAILED;
}

// 把内部 ImageBuffer 导出成 C 结构（malloc 一份，调用方负责释放）。
image_codec_vulkan_status exportImage(const ImageBuffer &src,
                                      image_codec_vulkan_image *out) {
  std::memset(out, 0, sizeof(*out));
  if (src.data.empty() || !src.valid()) {
    return IMAGE_CODEC_VULKAN_ERR_INTERNAL;
  }
  auto *data = static_cast<uint8_t *>(std::malloc(src.data.size()));
  if (data == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY;
  }
  std::memcpy(data, src.data.data(), src.data.size());
  out->width = static_cast<uint32_t>(src.width);
  out->height = static_cast<uint32_t>(src.height);
  out->channels = static_cast<uint32_t>(src.channels);
  out->data = data;
  out->stride = src.stride();
  out->data_size = src.data.size();
  return IMAGE_CODEC_VULKAN_OK;
}

// C 图像 -> 内部 ImageBuffer。C ABI 的 image 没有 stride 语义，内部却要 vector
// 所有权，所以这一步必然复制整幅图；大图上是 GB 级拷贝，调参（Tune）应走内部
// C++ 入口，别让这份拷贝污染计时。
bool toBuffer(const image_codec_vulkan_image *img, ImageBuffer *out) {
  if (img == nullptr || out == nullptr || img->data == nullptr) return false;
  if (img->width == 0 || img->height == 0) return false;
  if (img->channels < 1 || img->channels > 4) return false;
  const size_t stride =
      static_cast<size_t>(img->width) * static_cast<size_t>(img->channels);
  const size_t need = stride * static_cast<size_t>(img->height);
  if (img->data_size < need) return false;
  out->width = static_cast<int>(img->width);
  out->height = static_cast<int>(img->height);
  out->channels = static_cast<int>(img->channels);
  out->data.assign(img->data, img->data + need);
  return out->valid();
}

bool toBufferPair(const image_codec_vulkan_image *a,
                  const image_codec_vulkan_image *b, ImageBuffer *buf_a,
                  ImageBuffer *buf_b) {
  if (a == nullptr || b == nullptr) return false;
  if (a->width != b->width || a->height != b->height ||
      a->channels != b->channels) {
    // 指标不做隐式缩放/转通道：尺寸不同就是调用方的错。
    return false;
  }
  return toBuffer(a, buf_a) && toBuffer(b, buf_b);
}

// 编码参数默认值：JPEG q90 / TIFF LZW + Predictor=2 / PNG zlib 6 自适应滤波。
//
// 语义与 Image.Codec.OpenCL 一致：params == nullptr 才取默认值；传了结构体就按
// 字面值使用（例如 png_filter = 0 表示「固定 None」，不是「用默认」）。C 结构体
// 没有「未设置」标记，若在这里把 0 改写成默认值，调用方就永远选不到 None。
image_codec_vulkan_encode_params defaultParams() {
  image_codec_vulkan_encode_params p{};
  p.jpeg_quality = 90;
  p.jpeg_subsampling = 0;
  p.tiff_compression = 5;
  p.tiff_rows_per_strip = 0;
  p.tiff_tile_size = 0;
  p.tiff_predictor = 2;
  p.png_compression = 6;
  p.png_filter = -1;
  p.band_rows = 256;
  return p;
}

// 参数校验：非法组合立刻中止（换后端也一样错），不参与回退。
image_codec_vulkan_status
validateParams(int format, const image_codec_vulkan_encode_params &p,
               uint32_t channels) {
  switch (format) {
  case IMAGE_CODEC_VULKAN_FORMAT_TIFF:
    if (p.tiff_compression != 1 && p.tiff_compression != 5) {
      return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
    }
    if (p.tiff_rows_per_strip < 0) {
      return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
    }
    if (p.tiff_tile_size > 0 && p.tiff_tile_size % 16 != 0) {
      return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
    }
    if (p.tiff_predictor != 1 && p.tiff_predictor != 2) {
      return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
    }
    break;
  case IMAGE_CODEC_VULKAN_FORMAT_PNG:
    if (p.png_compression < 0 || p.png_compression > 9) {
      return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
    }
    if (p.png_filter < -1 || p.png_filter > 4) {
      return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
    }
    break;
  case IMAGE_CODEC_VULKAN_FORMAT_JPEG:
    if (p.jpeg_quality < 1 || p.jpeg_quality > 100) {
      return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
    }
    // 0 = 自动；其余只接受 444/422/420。
    if (p.jpeg_subsampling != 0 && p.jpeg_subsampling != 444 &&
        p.jpeg_subsampling != 422 && p.jpeg_subsampling != 420) {
      return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
    }
    // JPEG 没有 Alpha 样本：2/4 通道是「本后端表示不了」，让路由层换后端；
    // 0/5+ 才是调用方参数非法。
    if (channels == 1 || channels == 3) {
      return IMAGE_CODEC_VULKAN_OK;
    }
    return channels == 2 || channels == 4
               ? IMAGE_CODEC_VULKAN_ERR_UNSUPPORTED_FORMAT
               : IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  default:
    return IMAGE_CODEC_VULKAN_ERR_UNSUPPORTED_FORMAT;
  }
  return IMAGE_CODEC_VULKAN_OK;
}

// 指标入口共用的参数准备 + 失败分类。
image_codec_vulkan_status runMetric(
    image_codec_vulkan_codec *codec, const image_codec_vulkan_image *a,
    const image_codec_vulkan_image *b,
    const std::function<bool(const ImageBuffer &, const ImageBuffer &,
                             const image_codec::vulkan::metrics::Options &,
                             std::string *)> &fn) {
  ImageBuffer buf_a;
  ImageBuffer buf_b;
  if (!toBufferPair(a, b, &buf_a, &buf_b)) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  image_codec::vulkan::metrics::Options opt;
  opt.device_index = codec->device_index;
  opt.use_gpu = codec->use_gpu;
  opt.memory_limit = codec->memory_limit;

  std::string err;
  if (!fn(buf_a, buf_b, opt, &err)) {
    // 失败必须如实分类，不能一律收成 Internal：路由层的回退规则是
    // 「BackendUnavailable / UnsupportedFormat / OutOfMemory 换下一个后端」，
    // 「Internal 之类不阻止继续但记录下来」。把它压成 Internal 会让本机没有
    // fp64 时（指标算不了）连 CPU 指标都退不回去，直接变成一次硬失败。
    const image_codec_vulkan_status status =
        classifyFailure(err, /*encoding=*/false);
    switch (status) {
    case IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY:
    case IMAGE_CODEC_VULKAN_ERR_BACKEND_UNAVAILABLE:
    case IMAGE_CODEC_VULKAN_ERR_UNSUPPORTED_FORMAT:
      return status;
    default:
      return IMAGE_CODEC_VULKAN_ERR_INTERNAL;
    }
  }
  return IMAGE_CODEC_VULKAN_OK;
}

} // namespace

extern "C" {

const char *image_codec_vulkan_status_string(image_codec_vulkan_status status) {
  switch (status) {
  case IMAGE_CODEC_VULKAN_OK:
    return "success";
  case IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT:
    return "invalid argument";
  case IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY:
    return "out of memory";
  case IMAGE_CODEC_VULKAN_ERR_BACKEND_UNAVAILABLE:
    return "backend unavailable";
  case IMAGE_CODEC_VULKAN_ERR_UNSUPPORTED_FORMAT:
    return "unsupported format";
  case IMAGE_CODEC_VULKAN_ERR_DECODE_FAILED:
    return "decode failed";
  case IMAGE_CODEC_VULKAN_ERR_ENCODE_FAILED:
    return "encode failed";
  case IMAGE_CODEC_VULKAN_ERR_IO:
    return "I/O error";
  case IMAGE_CODEC_VULKAN_ERR_INTERNAL:
    return "internal error";
  default:
    return "unknown status";
  }
}

image_codec_vulkan_status
image_codec_vulkan_codec_create(image_codec_vulkan_codec **out_codec,
                                int device_index) {
  if (out_codec == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  *out_codec = nullptr;

  auto *codec = new (std::nothrow) image_codec_vulkan_codec();
  if (codec == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY;
  }
  codec->device_index = device_index;

  // 建一次设备验证「这台机器真的能用」并把快照留下。失败就明确报后端不可用，
  // 绝不允许句柄建成功、直到第一次 encode 才炸。
  Device device;
  std::string err;
  if (!device.init(device_index, &err)) {
    delete codec;
    return IMAGE_CODEC_VULKAN_ERR_BACKEND_UNAVAILABLE;
  }
  codec->caps = device.caps();
  codec->memory = device.memoryInfo();
  codec->note = "metrics accumulate in fp64 (requires shaderFloat64); "
                "TIFF LZW runs on CPU (serial dictionary), Predictor=2 on GPU; "
                "JPEG Huffman/container on CPU, DCT/color on GPU";

  *out_codec = codec;
  return IMAGE_CODEC_VULKAN_OK;
}

void image_codec_vulkan_codec_destroy(image_codec_vulkan_codec *codec) {
  delete codec;
}

image_codec_vulkan_status
image_codec_vulkan_get_capabilities(image_codec_vulkan_codec *codec,
                                    image_codec_vulkan_capabilities *out) {
  if (codec == nullptr || out == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  std::snprintf(out->device_name, sizeof(out->device_name), "%s",
                codec->caps.name.c_str());
  out->api_version = codec->caps.api_version;
  out->total_memory = codec->caps.total_bytes;
  // JPEG：容器与 Huffman 在 CPU，DCT/颜色变换在 GPU（本构建已实现）。
  out->jpeg_decode = 1;
  out->jpeg_encode = 1;
  out->tiff_decode = 1;
  out->tiff_encode = 1;
  out->png_decode = 1;
  out->png_encode = 1;
  out->tiff_predictor_gpu = 1;
  out->png_filter_gpu = 1;
  // LZW 字典是严格串行的，留在 CPU（GPU 版本只会更慢），因此如实报 0。
  out->tiff_lzw_gpu = 0;
  std::snprintf(out->note, sizeof(out->note), "%s", codec->note.c_str());
  return IMAGE_CODEC_VULKAN_OK;
}

image_codec_vulkan_status
image_codec_vulkan_set_use_gpu(image_codec_vulkan_codec *codec, int enable) {
  if (codec == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  codec->use_gpu = enable != 0;
  return IMAGE_CODEC_VULKAN_OK;
}

image_codec_vulkan_status
image_codec_vulkan_device_memory(int device_index,
                                 image_codec_vulkan_memory_info *out) {
  if (out == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  Device device;
  std::string err;
  if (!device.init(device_index, &err)) {
    return IMAGE_CODEC_VULKAN_ERR_BACKEND_UNAVAILABLE;
  }
  const MemoryInfo info = device.memoryInfo();
  out->total_bytes = info.total_bytes;
  out->free_bytes = info.free_bytes;
  out->max_alloc_bytes = info.max_alloc_bytes;
  return IMAGE_CODEC_VULKAN_OK;
}

image_codec_vulkan_status
image_codec_vulkan_get_memory_info(image_codec_vulkan_codec *codec,
                                   image_codec_vulkan_memory_info *out) {
  if (codec == nullptr || out == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  out->total_bytes = codec->memory.total_bytes;
  out->free_bytes = codec->memory.free_bytes;
  out->max_alloc_bytes = codec->memory.max_alloc_bytes;
  out->budget_bytes = codec->memory_limit;
  // 句柄不持有显存（每次操作内部临时建设备），故 used/peak 如实报 0。
  out->used_bytes = 0;
  out->peak_bytes = 0;
  return IMAGE_CODEC_VULKAN_OK;
}

image_codec_vulkan_status
image_codec_vulkan_set_memory_limit(image_codec_vulkan_codec *codec,
                                    size_t limit_bytes) {
  if (codec == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  codec->memory_limit = limit_bytes;
  codec->memory.budget_bytes = limit_bytes;
  return IMAGE_CODEC_VULKAN_OK;
}

image_codec_vulkan_status
image_codec_vulkan_probe(const void *data, size_t size, int *out_format) {
  if (out_format == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  *out_format = IMAGE_CODEC_VULKAN_FORMAT_UNKNOWN;
  // 最短的魔数是 JPEG 的 3 字节（FF D8 FF）；PNG(8)/TIFF(4) 各自在
  // 自己的 has*Signature() 里再判长度，这里只做统一的下限。
  if (data == nullptr || size < 3) {
    return IMAGE_CODEC_VULKAN_OK;
  }
  const auto *p = static_cast<const uint8_t *>(data);
  // 三种魔数互不冲突：PNG 8 字节、JPEG 3 字节（FF D8 FF）、TIFF 4 字节。
  if (image_codec::vulkan::png::hasPngSignature(p, size)) {
    *out_format = IMAGE_CODEC_VULKAN_FORMAT_PNG;
  } else if (image_codec::vulkan::jpeg::hasJpegSignature(p, size)) {
    *out_format = IMAGE_CODEC_VULKAN_FORMAT_JPEG;
  } else if (image_codec::vulkan::tiff::hasTiffSignature(p, size)) {
    *out_format = IMAGE_CODEC_VULKAN_FORMAT_TIFF;
  }
  return IMAGE_CODEC_VULKAN_OK;
}

image_codec_vulkan_status
image_codec_vulkan_decode(image_codec_vulkan_codec *codec, int format,
                          const void *in, size_t in_len,
                          image_codec_vulkan_image *out) {
  if (codec == nullptr || out == nullptr || in == nullptr || in_len == 0) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  int fmt = format;
  if (fmt == IMAGE_CODEC_VULKAN_FORMAT_UNKNOWN) {
    image_codec_vulkan_probe(in, in_len, &fmt);
  }

  const auto *bytes = static_cast<const uint8_t *>(in);
  ImageBuffer img;
  std::string err;
  bool ok = false;
  switch (fmt) {
  case IMAGE_CODEC_VULKAN_FORMAT_TIFF:
    ok = image_codec::vulkan::tiff::decodeTiff(bytes, in_len, &img, &err);
    break;
  case IMAGE_CODEC_VULKAN_FORMAT_PNG:
    ok = image_codec::vulkan::png::decodePng(bytes, in_len, &img, &err);
    break;
  case IMAGE_CODEC_VULKAN_FORMAT_JPEG: {
    image_codec::vulkan::jpeg::JpegOptions opt;
    opt.use_gpu = codec->use_gpu;
    opt.device_index = codec->device_index;
    opt.memory_limit = codec->memory_limit;
    bool gpu_used = false;
    ok = image_codec::vulkan::jpeg::decodeJpeg(bytes, in_len, opt, &img,
                                               &gpu_used, &err);
    break;
  }
  default:
    return IMAGE_CODEC_VULKAN_ERR_UNSUPPORTED_FORMAT;
  }
  if (!ok) {
    return classifyFailure(err, /*encoding=*/false);
  }
  return exportImage(img, out);
}

image_codec_vulkan_status
image_codec_vulkan_encode(image_codec_vulkan_codec *codec, int format,
                          const image_codec_vulkan_image *img,
                          const image_codec_vulkan_encode_params *params,
                          void *out, size_t *out_len, size_t out_capacity) {
  if (codec == nullptr || img == nullptr || out_len == nullptr ||
      img->data == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  if (img->width == 0 || img->height == 0 || img->channels == 0 ||
      img->channels > 4) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }

  ImageBuffer src;
  if (!toBuffer(img, &src)) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }

  image_codec_vulkan_encode_params p = defaultParams();
  if (params != nullptr) {
    p = *params;
  }
  if (p.band_rows <= 0) p.band_rows = 256;
  // 0 是「未指定」的唯一含义（1..100 都是合法质量），与 CUDA 后端一致。
  if (p.jpeg_quality == 0) p.jpeg_quality = 90;
  // 0 是「未指定」的唯一含义（1/2 都是合法值），与头文件里写的「0 取默认 2」一致。
  if (p.tiff_predictor == 0) p.tiff_predictor = 2;

  const image_codec_vulkan_status param_status =
      validateParams(format, p, img->channels);
  if (param_status != IMAGE_CODEC_VULKAN_OK) {
    return param_status;
  }

  std::vector<uint8_t> encoded;
  std::string err;
  bool ok = false;
  switch (format) {
  case IMAGE_CODEC_VULKAN_FORMAT_TIFF: {
    image_codec::vulkan::tiff::TiffEncodeOptions opt;
    opt.compression = p.tiff_compression;
    opt.rows_per_strip = p.tiff_rows_per_strip;
    opt.tile_size = p.tiff_tile_size;
    opt.predictor = p.tiff_predictor != 1;
    opt.use_gpu = codec->use_gpu;
    opt.band_rows = p.band_rows > 0 ? p.band_rows : 256;
    opt.memory_limit = codec->memory_limit;
    opt.device_index = codec->device_index;
    bool gpu_used = false;
    ok = image_codec::vulkan::tiff::encodeTiff(src, opt, &encoded, &gpu_used,
                                               &err);
    break;
  }
  case IMAGE_CODEC_VULKAN_FORMAT_PNG: {
    image_codec::vulkan::png::PngEncodeOptions opt;
    opt.compression_level = p.png_compression;
    opt.filter = p.png_filter;
    opt.use_gpu = codec->use_gpu;
    opt.band_rows = p.band_rows > 0 ? p.band_rows : 256;
    opt.memory_limit = codec->memory_limit;
    opt.device_index = codec->device_index;
    bool gpu_used = false;
    ok = image_codec::vulkan::png::encodePng(src, opt, &encoded, &gpu_used,
                                             &err);
    break;
  }
  case IMAGE_CODEC_VULKAN_FORMAT_JPEG: {
    image_codec::vulkan::jpeg::JpegOptions opt;
    opt.quality = p.jpeg_quality;
    opt.subsampling = p.jpeg_subsampling;
    opt.use_gpu = codec->use_gpu;
    opt.device_index = codec->device_index;
    opt.memory_limit = codec->memory_limit;
    bool gpu_used = false;
    ok = image_codec::vulkan::jpeg::encodeJpeg(src, opt, &encoded, &gpu_used,
                                               &err);
    break;
  }
  default:
    return IMAGE_CODEC_VULKAN_ERR_UNSUPPORTED_FORMAT;
  }
  if (!ok) {
    return classifyFailure(err, /*encoding=*/true);
  }

  *out_len = encoded.size();
  if (out == nullptr) {
    // 先查后写：返回所需大小，调用方按此重新调用。
    return IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY;
  }
  if (out_capacity < encoded.size()) {
    return IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY;
  }
  std::memcpy(out, encoded.data(), encoded.size());
  return IMAGE_CODEC_VULKAN_OK;
}

image_codec_vulkan_status image_codec_vulkan_gpu_stage(
    image_codec_vulkan_codec *codec, const image_codec_vulkan_image *in,
    image_codec_vulkan_image *out) {
  if (codec == nullptr || in == nullptr || out == nullptr || in->data == nullptr ||
      in->width == 0 || in->height == 0 || in->channels == 0 ||
      in->channels > 4) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  const size_t row_bytes = static_cast<size_t>(in->width) * in->channels;
  if (in->height > static_cast<size_t>(-1) / row_bytes) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  const size_t bytes = row_bytes * in->height;
  if (in->stride != 0 && in->stride < row_bytes) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  std::vector<uint8_t> packed;
  const uint8_t *source = in->data;
  if (in->stride != 0 && in->stride != row_bytes) {
    packed.resize(bytes);
    for (uint32_t y = 0; y < in->height; ++y) {
      std::memcpy(packed.data() + static_cast<size_t>(y) * row_bytes,
                  in->data + static_cast<size_t>(y) * in->stride, row_bytes);
    }
    source = packed.data();
  }

  Device device;
  std::string err;
  if (!device.init(codec->device_index, &err)) {
    return IMAGE_CODEC_VULKAN_ERR_BACKEND_UNAVAILABLE;
  }
  device.setMemoryLimit(codec->memory_limit);
  Buffer buffer(&device, bytes);
  if (!buffer.ok() || !buffer.upload(source, bytes)) {
    return device.takeExhausted() ? IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY
                                  : IMAGE_CODEC_VULKAN_ERR_INTERNAL;
  }
  std::vector<uint8_t> result(bytes);
  if (!buffer.download(result.data(), bytes)) {
    return device.takeExhausted() ? IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY
                                  : IMAGE_CODEC_VULKAN_ERR_INTERNAL;
  }
  out->data = static_cast<uint8_t *>(std::malloc(bytes));
  if (out->data == nullptr) return IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY;
  std::memcpy(out->data, result.data(), bytes);
  out->width = in->width;
  out->height = in->height;
  out->channels = in->channels;
  out->stride = row_bytes;
  out->data_size = bytes;
  return IMAGE_CODEC_VULKAN_OK;
}

// 注意：下面三个入口都不带错误字符串（与另外两个后端一致）。需要具体原因
// （例如「显存上限太低」）时走内部 C++ 入口，它带 std::string* err。

image_codec_vulkan_status image_codec_vulkan_metric_mse(
    image_codec_vulkan_codec *codec, const image_codec_vulkan_image *a,
    const image_codec_vulkan_image *b, double *out_value) {
  if (codec == nullptr || out_value == nullptr || a == nullptr ||
      b == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  *out_value = 0.0;
  return runMetric(codec, a, b,
                   [out_value](const ImageBuffer &x, const ImageBuffer &y,
                               const image_codec::vulkan::metrics::Options &o,
                               std::string *err) {
                     return image_codec::vulkan::metrics::mse(x, y, o,
                                                              out_value, err);
                   });
}

image_codec_vulkan_status image_codec_vulkan_metric_psnr(
    image_codec_vulkan_codec *codec, const image_codec_vulkan_image *a,
    const image_codec_vulkan_image *b, double *out_value) {
  if (codec == nullptr || out_value == nullptr || a == nullptr ||
      b == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  *out_value = 0.0;
  return runMetric(codec, a, b,
                   [out_value](const ImageBuffer &x, const ImageBuffer &y,
                               const image_codec::vulkan::metrics::Options &o,
                               std::string *err) {
                     return image_codec::vulkan::metrics::psnr(x, y, o,
                                                               out_value, err);
                   });
}

image_codec_vulkan_status image_codec_vulkan_metric_ssim(
    image_codec_vulkan_codec *codec, const image_codec_vulkan_image *a,
    const image_codec_vulkan_image *b, image_codec_vulkan_ssim *out) {
  if (codec == nullptr || out == nullptr || a == nullptr || b == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));
  image_codec::vulkan::metrics::SsimResult result;
  const image_codec_vulkan_status status = runMetric(
      codec, a, b,
      [&result](const ImageBuffer &x, const ImageBuffer &y,
                const image_codec::vulkan::metrics::Options &o,
                std::string *err) {
        return image_codec::vulkan::metrics::ssim(x, y, o, &result, err);
      });
  if (status != IMAGE_CODEC_VULKAN_OK) return status;
  out->ssim = result.ssim;
  out->ms_ssim = result.ms_ssim;
  out->levels = result.levels;
  return IMAGE_CODEC_VULKAN_OK;
}

void image_codec_vulkan_image_free(image_codec_vulkan_image *img) {
  if (img == nullptr) return;
  std::free(img->data);
  std::memset(img, 0, sizeof(*img));
}

image_codec_vulkan_status image_codec_vulkan_device_list(char ***names,
                                                         int *count) {
  if (names == nullptr || count == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT;
  }
  *names = nullptr;
  *count = 0;

  // 逐个下标尝试建设备：能建成功的就是「本后端真的能用」的设备。
  // Device::init 在越界时给的错误固定以 "out of range" 开头，据此收尾；
  // 上限 16 只是防御性上限（现实中一台机器不可能有 16 块 compute GPU）。
  // 没有 fp64 的卡也列入：编解码不需要 fp64，只有指标会回退 CPU。
  std::vector<std::string> list;
  for (int index = 0; index < 16; ++index) {
    Device device;
    std::string err;
    if (!device.init(index, &err)) {
      if (toLower(err).find("out of range") != std::string::npos) break;
      // 这一块卡建不出来（没有 compute 队列等）：跳过，别让它把后面的卡藏起来。
      continue;
    }
    char buf[384];
    std::snprintf(buf, sizeof(buf), "%d: %s (api %u.%u, %zu MiB)", index,
                  device.caps().name.c_str(),
                  VK_API_VERSION_MAJOR(device.caps().api_version),
                  VK_API_VERSION_MINOR(device.caps().api_version),
                  device.caps().total_bytes >> 20);
    list.emplace_back(buf);
  }

  if (list.empty()) {
    return IMAGE_CODEC_VULKAN_OK;
  }
  auto **out = static_cast<char **>(std::calloc(list.size(), sizeof(char *)));
  if (out == nullptr) {
    return IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY;
  }
  for (size_t i = 0; i < list.size(); ++i) {
    out[i] = dupString(list[i].c_str());
    if (out[i] == nullptr) {
      for (size_t k = 0; k < i; ++k) std::free(out[k]);
      std::free(out);
      return IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY;
    }
  }
  *names = out;
  *count = static_cast<int>(list.size());
  return IMAGE_CODEC_VULKAN_OK;
}

void image_codec_vulkan_device_list_free(char **names, int count) {
  if (names == nullptr) return;
  for (int i = 0; i < count; ++i) std::free(names[i]);
  std::free(names);
}

} // extern "C"
