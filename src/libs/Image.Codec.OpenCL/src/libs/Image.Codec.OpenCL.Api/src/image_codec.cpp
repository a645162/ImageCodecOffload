/*
 * File: image_codec.cpp
 * Brief: Image codec.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/api/image_codec.cpp - 对外 C API 实现。
//
// 本层直接调用各模块的**内存缓冲**入口（png::decodePng / tiffDecodeFromBuffer /
// jpeg_decode_buffer 等），不再经过临时 BMP 落盘：
//   * 中间文件的代价随图像线性增长（1.9 GB 的图要白写白读一遍），而收益是零；
//   * BMP 表达不了 2 通道（灰度+Alpha），落盘一轮就会让 RGBA 这条路断掉。
// 文件级入口（image_codec_*_decode/encode，见各 *_test.h）仍然保留，
// 它们只是 CLI 的便捷封装，不是 C API 的通路。
#include "Image/Codec/OpenCL/image_codec.h"

#include "../../Image.Codec.OpenCL.Core/src/ocl_device.h"
#include "../../Image.Codec.OpenCL.Core/src/ocl_program.h"
#include "../../Image.Codec.OpenCL.Common/src/image_buffer.h"
#include "../../Image.Codec.OpenCL.Jpeg/src/jpeg_codec.h"
#include "../../Image.Codec.OpenCL.Png/src/png_codec.h"
#include "../../Image.Codec.OpenCL.Tiff/src/lzw_codec.h"
#include "../../Image.Codec.OpenCL.Metrics/src/metrics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using image_codec::OclDevice;
using image_codec::OclDeviceInfo;

// 不透明句柄实际结构。
struct image_codec_codec {
  int backend; // image_codec_backend 的整数值
};

namespace {

// 2 通道（灰度+Alpha）只有 PNG / TIFF 能表示；JPEG 没有 Alpha，必须在进后端
// 之前就明确拒绝，而不是把 Alpha 丢掉装作成功。
bool channelsAllowed(image_codec_format format, int channels) {
  if (channels < 1 || channels > 4) return false;
  if (format == IMAGE_CODEC_FORMAT_JPEG) return channels == 1 || channels == 3;
  return true;
}

bool resourceFailure(const std::string &error) {
  return error.find("CL_MEM_OBJECT_ALLOCATION_FAILURE") != std::string::npos ||
         error.find("CL_OUT_OF_RESOURCES") != std::string::npos ||
         error.find("CL_OUT_OF_HOST_MEMORY") != std::string::npos;
}

} // namespace

// 下面这些辅助函数必须放在 extern "C" 之外：C 语言链接的符号没有名字修饰，
// 放在 extern "C" 里的匿名命名空间并**不会**变成内部链接（GCC 实测会导出
// `T metricArgsValid`），两个后端的 C API 一起链接时就会在链接期撞车。
namespace {

bool metricArgsValid(const image_codec_codec *codec, const image_codec_image *a,
                     const image_codec_image *b) {
  if (codec == nullptr || a == nullptr || b == nullptr) {
    return false;
  }
  if (a->data == nullptr || b->data == nullptr) {
    return false;
  }
  if (a->width == 0 || a->height == 0 || a->channels == 0) {
    return false;
  }
  return a->width == b->width && a->height == b->height &&
         a->channels == b->channels;
}

image_codec::ocl::metrics::ImageView toView(const image_codec_image *img) {
  image_codec::ocl::metrics::ImageView v;
  v.data = img->data;
  v.width = static_cast<int>(img->width);
  v.height = static_cast<int>(img->height);
  v.channels = static_cast<int>(img->channels);
  v.stride = img->stride;
  return v;
}

// 后端为 CPU 时走参考实现：语义与编解码一致（CPU 后端 = 不碰设备）。
image_codec::ocl::metrics::Options
metricOptions(const image_codec_codec *codec) {
  image_codec::ocl::metrics::Options opt;
  opt.use_gpu = codec->backend != IMAGE_CODEC_BACKEND_CPU;
  return opt;
}

} // namespace

extern "C" {

// ---- 状态码 ----
const char *image_codec_status_string(image_codec_status status) {
  switch (status) {
  case IMAGE_CODEC_OK:
    return "success";
  case IMAGE_CODEC_ERR_INVALID_ARGUMENT:
    return "invalid argument";
  case IMAGE_CODEC_ERR_OUT_OF_MEMORY:
    return "out of memory";
  case IMAGE_CODEC_ERR_BACKEND_UNAVAILABLE:
    return "backend unavailable";
  case IMAGE_CODEC_ERR_UNSUPPORTED_FORMAT:
    return "unsupported format";
  case IMAGE_CODEC_ERR_DECODE_FAILED:
    return "decode failed";
  case IMAGE_CODEC_ERR_ENCODE_FAILED:
    return "encode failed";
  case IMAGE_CODEC_ERR_IO:
    return "I/O error";
  case IMAGE_CODEC_ERR_INTERNAL:
    return "internal error";
  default:
    return "unknown status";
  }
}

// ---- 生命周期 ----
image_codec_status image_codec_codec_create(image_codec_codec **out_codec,
                                            image_codec_backend backend) {
  if (out_codec == nullptr) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  *out_codec = nullptr;
  if (backend != IMAGE_CODEC_BACKEND_OPENCL &&
      backend != IMAGE_CODEC_BACKEND_CPU) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  image_codec_codec *c =
      static_cast<image_codec_codec *>(std::malloc(sizeof(image_codec_codec)));
  if (c == nullptr) {
    return IMAGE_CODEC_ERR_OUT_OF_MEMORY;
  }
  c->backend = static_cast<int>(backend);
  *out_codec = c;
  return IMAGE_CODEC_OK;
}

void image_codec_codec_destroy(image_codec_codec *codec) { std::free(codec); }

// ---- 解码 ----
image_codec_status image_codec_codec_decode(image_codec_codec *codec,
                                            image_codec_format format,
                                            const uint8_t *in, size_t in_len,
                                            image_codec_image *out) {
  if (codec == nullptr || out == nullptr) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (in == nullptr || in_len == 0) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));

  // 模块内部的后端取值与 C API 枚举并不统一（历史原因）：JPEG 用 0/1，
  // TIFF 用 0/2。这里显式换算，别让调用方去猜。
  const bool cpu = codec->backend == IMAGE_CODEC_BACKEND_CPU;

  image_codec::ImageBuffer img;
  std::string err;

  switch (format) {
  case IMAGE_CODEC_FORMAT_JPEG: {
    // jpeg_decode_buffer 要 vector，且它是唯一一个「必须整段持有」的入口
    // （JPEG 的熵解码需要回填 offset）。其余格式直接吃裸指针。
    std::vector<uint8_t> file(in, in + in_len);
    image_codec::jpeg::JpegDecodeResult res;
    if (image_codec::jpeg::jpeg_decode_buffer(file, cpu ? 1 : 0, &res) != 0) {
      return resourceFailure(res.error) ? IMAGE_CODEC_ERR_OUT_OF_MEMORY
                                        : IMAGE_CODEC_ERR_DECODE_FAILED;
    }
    img.width = res.width;
    img.height = res.height;
    img.channels = res.channels;
    img.data = std::move(res.rgb);
    break;
  }
  case IMAGE_CODEC_FORMAT_TIFF: {
    int w = 0, h = 0, c = 0;
    if (image_codec::tiff::tiffDecodeFromBuffer(in, in_len, w, h, c,
                                                img.data) != 0) {
      return IMAGE_CODEC_ERR_DECODE_FAILED;
    }
    img.width = w;
    img.height = h;
    img.channels = c;
    break;
  }
  case IMAGE_CODEC_FORMAT_PNG: {
    if (!image_codec::ocl::png::decodePng(in, in_len, &img, &err)) {
      return IMAGE_CODEC_ERR_DECODE_FAILED;
    }
    break;
  }
  default:
    return IMAGE_CODEC_ERR_UNSUPPORTED_FORMAT;
  }

  if (!img.valid()) {
    return IMAGE_CODEC_ERR_DECODE_FAILED;
  }

  out->width = static_cast<uint32_t>(img.width);
  out->height = static_cast<uint32_t>(img.height);
  out->channels = static_cast<uint32_t>(img.channels);
  out->stride = img.stride();
  out->data_size = img.size();
  out->data = static_cast<uint8_t *>(std::malloc(out->data_size));
  if (out->data == nullptr) {
    std::memset(out, 0, sizeof(*out));
    return IMAGE_CODEC_ERR_OUT_OF_MEMORY;
  }
  std::memcpy(out->data, img.data.data(), out->data_size);
  return IMAGE_CODEC_OK;
}

// ---- 编码 ----
image_codec_status
image_codec_codec_encode(image_codec_codec *codec, image_codec_format format,
                         const image_codec_image *img,
                         const image_codec_encode_params *params, uint8_t *out,
                         size_t *out_len, size_t out_capacity) {
  if (codec == nullptr || img == nullptr || out == nullptr ||
      out_len == nullptr) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (img->data == nullptr || img->width == 0 || img->height == 0) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  // 通道 1..4 都接受，但 JPEG 没有 Alpha 样本：2/4 通道必须明确拒绝。
  // 悄悄丢掉 Alpha 也是一种「成功」，只是成功得很危险。
  if (!channelsAllowed(format, static_cast<int>(img->channels))) {
    return format == IMAGE_CODEC_FORMAT_JPEG && img->channels != 0 &&
                   img->channels <= 4
               ? IMAGE_CODEC_ERR_UNSUPPORTED_FORMAT
               : IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }

  const int backend = codec->backend;
  const int jpeg_quality = params != nullptr ? params->jpeg_quality : 85;
  const int jpeg_subsampling = params != nullptr ? params->jpeg_subsampling : 0;
  const int tiff_compression = params != nullptr ? params->tiff_compression : 5;
  const int tiff_rows_per_strip =
      params != nullptr ? params->tiff_rows_per_strip : 0;
  const int png_compression = params != nullptr ? params->png_compression : 6;
  const int png_filter = params != nullptr ? params->png_filter : -1;

  // 参数校验。
  switch (format) {
  case IMAGE_CODEC_FORMAT_JPEG:
    if (jpeg_quality < 1 || jpeg_quality > 100) {
      return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
    }
    // 0 = 自动；其余只接受 444/422/420。
    if (jpeg_subsampling != 0 && jpeg_subsampling != 444 &&
        jpeg_subsampling != 422 && jpeg_subsampling != 420) {
      return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
    }
    break;
  case IMAGE_CODEC_FORMAT_TIFF:
    if (tiff_compression != 1 && tiff_compression != 5) {
      return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
    }
    if (tiff_rows_per_strip < 0) {
      return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
    }
    break;
  case IMAGE_CODEC_FORMAT_PNG:
    if (png_compression < 0 || png_compression > 9) {
      return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
    }
    if (png_filter < -1 || png_filter > 4) {
      return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
    }
    break;
  default:
    return IMAGE_CODEC_ERR_UNSUPPORTED_FORMAT;
  }

  // 打包像素行（处理 stride != width*channels 的情况）。
  const size_t row_bytes = static_cast<size_t>(img->width) * img->channels;
  std::vector<uint8_t> packed;
  const uint8_t *rgb = img->data;
  if (img->stride != 0 && img->stride != row_bytes) {
    packed.resize(row_bytes * img->height);
    for (uint32_t y = 0; y < img->height; y++) {
      std::memcpy(packed.data() + static_cast<size_t>(y) * row_bytes,
                  img->data + static_cast<size_t>(y) * img->stride, row_bytes);
    }
    rgb = packed.data();
  }

  std::vector<uint8_t> encoded;
  std::string err;
  const bool cpu = backend == IMAGE_CODEC_BACKEND_CPU;

  switch (format) {
  case IMAGE_CODEC_FORMAT_JPEG: {
    const std::vector<uint8_t> pixels(rgb, rgb + row_bytes * img->height);
    if (image_codec::jpeg::jpeg_encode_buffer(
            pixels, static_cast<int>(img->width), static_cast<int>(img->height),
            static_cast<int>(img->channels), jpeg_quality, jpeg_subsampling,
            cpu ? 1 : 0, &encoded, &err) != 0) {
      return resourceFailure(err) ? IMAGE_CODEC_ERR_OUT_OF_MEMORY
                                  : IMAGE_CODEC_ERR_ENCODE_FAILED;
    }
    break;
  }
  case IMAGE_CODEC_FORMAT_TIFF: {
    if (image_codec::tiff::tiffEncodeToBuffer(
            static_cast<int>(img->width), static_cast<int>(img->height),
            static_cast<int>(img->channels), rgb, tiff_compression,
            tiff_rows_per_strip, cpu ? 2 : 0, encoded) != 0) {
      return IMAGE_CODEC_ERR_ENCODE_FAILED;
    }
    break;
  }
  case IMAGE_CODEC_FORMAT_PNG: {
    image_codec::ImageBuffer view;
    view.width = static_cast<int>(img->width);
    view.height = static_cast<int>(img->height);
    view.channels = static_cast<int>(img->channels);
    view.data.assign(rgb, rgb + row_bytes * img->height);

    image_codec::ocl::png::PngEncodeOptions options;
    options.use_gpu = !cpu;
    options.filter = png_filter;
    options.compression_level = png_compression;
    bool gpu_used = false;
    if (!image_codec::ocl::png::encodePng(view, options, &encoded, &gpu_used,
                                          &err)) {
      return resourceFailure(err) ? IMAGE_CODEC_ERR_OUT_OF_MEMORY
                                  : IMAGE_CODEC_ERR_ENCODE_FAILED;
    }
    break;
  }
  default:
    return IMAGE_CODEC_ERR_UNSUPPORTED_FORMAT;
  }

  if (encoded.size() > out_capacity) {
    *out_len = encoded.size(); // 告知所需大小，支持先查后写。
    return IMAGE_CODEC_ERR_OUT_OF_MEMORY;
  }
  std::memcpy(out, encoded.data(), encoded.size());
  *out_len = encoded.size();
  return IMAGE_CODEC_OK;
}

// ---- 释放 ----
void image_codec_image_free(image_codec_image *img) {
  if (img == nullptr) {
    return;
  }
  if (img->data != nullptr) {
    std::free(img->data);
  }
  std::memset(img, 0, sizeof(*img));
}

image_codec_status image_codec_codec_gpu_stage(image_codec_codec *codec,
                                               const image_codec_image *in,
                                               image_codec_image *out) {
  if (codec == nullptr || in == nullptr || out == nullptr || in->data == nullptr ||
      in->width == 0 || in->height == 0 || in->channels == 0 ||
      in->channels > 4) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (codec->backend == IMAGE_CODEC_BACKEND_CPU) {
    return IMAGE_CODEC_ERR_BACKEND_UNAVAILABLE;
  }
  std::memset(out, 0, sizeof(*out));
  const size_t row_bytes = static_cast<size_t>(in->width) * in->channels;
  const size_t bytes = row_bytes * in->height;
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

  image_codec::OclDevice device;
  if (device.init() != CL_SUCCESS) return IMAGE_CODEC_ERR_BACKEND_UNAVAILABLE;
  static constexpr const char *kCopyKernel = R"CLC(
__kernel void copy_tiles(__global const uchar* src, __global uchar* dst,
                         uint count) {
  const uint i = (uint)get_global_id(0);
  if (i < count) dst[i] = src[i];
}
)CLC";
  cl_program program = nullptr;
  std::string build_log;
  cl_int err = image_codec::OclProgram::build(
      device.context(), device.device(), kCopyKernel, "", &program, &build_log);
  if (err != CL_SUCCESS) return IMAGE_CODEC_ERR_INTERNAL;
  cl_kernel kernel = nullptr;
  err = image_codec::OclProgram::createKernel(program, "copy_tiles", &kernel);
  if (err != CL_SUCCESS) {
    image_codec::OclProgram::release(program);
    return IMAGE_CODEC_ERR_INTERNAL;
  }
  cl_mem src = clCreateBuffer(device.context(), CL_MEM_READ_ONLY |
                                  CL_MEM_COPY_HOST_PTR,
                              bytes, const_cast<uint8_t *>(source), &err);
  cl_mem dst = err == CL_SUCCESS
                   ? clCreateBuffer(device.context(), CL_MEM_WRITE_ONLY, bytes,
                                    nullptr, &err)
                   : nullptr;
  if (err != CL_SUCCESS || src == nullptr || dst == nullptr) {
    if (src != nullptr) clReleaseMemObject(src);
    if (dst != nullptr) clReleaseMemObject(dst);
    image_codec::OclProgram::release(kernel);
    image_codec::OclProgram::release(program);
    return (err == CL_MEM_OBJECT_ALLOCATION_FAILURE ||
            err == CL_OUT_OF_RESOURCES || err == CL_OUT_OF_HOST_MEMORY)
               ? IMAGE_CODEC_ERR_OUT_OF_MEMORY
               : IMAGE_CODEC_ERR_INTERNAL;
  }
  const cl_uint count = static_cast<cl_uint>(bytes);
  err = clSetKernelArg(kernel, 0, sizeof(cl_mem), &src);
  err |= clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst);
  err |= clSetKernelArg(kernel, 2, sizeof(cl_uint), &count);
  const size_t global = bytes;
  if (err == CL_SUCCESS)
    err = clEnqueueNDRangeKernel(device.queue(), kernel, 1, nullptr, &global,
                                 nullptr, 0, nullptr, nullptr);
  std::vector<uint8_t> result(bytes);
  if (err == CL_SUCCESS)
    err = clEnqueueReadBuffer(device.queue(), dst, CL_TRUE, 0, bytes,
                              result.data(), 0, nullptr, nullptr);
  clReleaseMemObject(src);
  clReleaseMemObject(dst);
  image_codec::OclProgram::release(kernel);
  image_codec::OclProgram::release(program);
  if (err != CL_SUCCESS) {
    return (err == CL_MEM_OBJECT_ALLOCATION_FAILURE ||
            err == CL_OUT_OF_RESOURCES || err == CL_OUT_OF_HOST_MEMORY)
               ? IMAGE_CODEC_ERR_OUT_OF_MEMORY
               : IMAGE_CODEC_ERR_INTERNAL;
  }
  out->width = in->width;
  out->height = in->height;
  out->channels = in->channels;
  out->stride = row_bytes;
  out->data_size = bytes;
  out->data = static_cast<uint8_t *>(std::malloc(bytes));
  if (out->data == nullptr) return IMAGE_CODEC_ERR_OUT_OF_MEMORY;
  std::memcpy(out->data, result.data(), bytes);
  return IMAGE_CODEC_OK;
}

// ---- 画质指标 ----
//
// 本层只做 image_codec_image -> ImageView 的转换（非拥有视图，不复制像素），
// 具体实现在 Image.Codec.OpenCL.Metrics 里。

image_codec_status image_codec_metric_mse(image_codec_codec *codec,
                                          const image_codec_image *a,
                                          const image_codec_image *b,
                                          double *out_value) {
  if (out_value == nullptr || !metricArgsValid(codec, a, b)) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  std::string err;
  if (!image_codec::ocl::metrics::mse(toView(a), toView(b),
                                      metricOptions(codec), out_value, &err)) {
    // 与编解码一致：C API 只回状态码，具体原因走内部 C++ 入口。
    return IMAGE_CODEC_ERR_INTERNAL;
  }
  return IMAGE_CODEC_OK;
}

image_codec_status image_codec_metric_psnr(image_codec_codec *codec,
                                           const image_codec_image *a,
                                           const image_codec_image *b,
                                           double *out_value) {
  if (out_value == nullptr || !metricArgsValid(codec, a, b)) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  std::string err;
  if (!image_codec::ocl::metrics::psnr(toView(a), toView(b),
                                       metricOptions(codec), out_value, &err)) {
    return IMAGE_CODEC_ERR_INTERNAL;
  }
  return IMAGE_CODEC_OK;
}

image_codec_status image_codec_metric_ssim(image_codec_codec *codec,
                                           const image_codec_image *a,
                                           const image_codec_image *b,
                                           image_codec_ssim *out) {
  if (out == nullptr || !metricArgsValid(codec, a, b)) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  std::memset(out, 0, sizeof(*out));

  image_codec::ocl::metrics::SsimResult r;
  std::string err;
  if (!image_codec::ocl::metrics::ssim(toView(a), toView(b),
                                       metricOptions(codec), &r, &err)) {
    return IMAGE_CODEC_ERR_INTERNAL;
  }
  out->ssim = r.ssim;
  out->ms_ssim = r.ms_ssim;
  out->levels = r.levels;
  return IMAGE_CODEC_OK;
}

// ---- 设备枚举 ----
image_codec_status image_codec_device_list(char ***names, int *count) {
  if (names == nullptr || count == nullptr) {
    return IMAGE_CODEC_ERR_INVALID_ARGUMENT;
  }
  *names = nullptr;
  *count = 0;

  std::vector<OclDeviceInfo> devices;
  try {
    devices = OclDevice::enumerate();
  } catch (...) {
    return IMAGE_CODEC_ERR_INTERNAL;
  }
  if (devices.empty()) {
    return IMAGE_CODEC_OK;
  }

  char **arr =
      static_cast<char **>(std::calloc(devices.size(), sizeof(char *)));
  if (arr == nullptr) {
    return IMAGE_CODEC_ERR_OUT_OF_MEMORY;
  }
  for (size_t i = 0; i < devices.size(); i++) {
    const OclDeviceInfo &d = devices[i];
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s | %s | %s | CL %u.%u",
                  d.platformName.c_str(), d.deviceName.c_str(),
                  d.vendor.c_str(), d.major, d.minor);
    const size_t len = std::strlen(buf);
    char *name = static_cast<char *>(std::malloc(len + 1));
    if (name == nullptr) {
      image_codec_device_list_free(arr, static_cast<int>(i));
      return IMAGE_CODEC_ERR_OUT_OF_MEMORY;
    }
    std::memcpy(name, buf, len + 1);
    arr[i] = name;
  }
  *names = arr;
  *count = static_cast<int>(devices.size());
  return IMAGE_CODEC_OK;
}

void image_codec_device_list_free(char **names, int count) {
  if (names == nullptr) {
    return;
  }
  for (int i = 0; i < count; i++) {
    if (names[i] != nullptr) {
      std::free(names[i]);
    }
  }
  std::free(names);
}

} // extern "C"
