/*
 * File: cuda_backend.cpp
 * Brief: Cuda backend.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// cuda_backend.cpp - ImageCodecCUDA 的 ICodecBackend 适配。
//
// 与 OpenCL 适配层同样只做三件事：结构体转换、状态码映射、失败原因补后端名。
// 多出来的一件事是**显存预算**：CUDA 侧可以在创建句柄后设置上限，路由层
// 因此可以在「图太大」时提前失败，而不是让驱动把整机拖进 swap。
#include "cuda_backend.h"

#ifdef IMAGECODEC_OFFLOAD_WITH_CUDA

#include <ImageCodecCUDA/image_codec_cuda.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace image_codec {
namespace offload {

namespace {

Status mapStatus(image_codec_cuda_status status) {
  switch (status) {
    case IMAGE_CODEC_CUDA_OK:
      return Status::Ok;
    case IMAGE_CODEC_CUDA_ERR_INVALID_ARGUMENT:
      return Status::InvalidArgument;
    case IMAGE_CODEC_CUDA_ERR_OUT_OF_MEMORY:
      return Status::OutOfMemory;
    case IMAGE_CODEC_CUDA_ERR_BACKEND_UNAVAILABLE:
      return Status::BackendUnavailable;
    case IMAGE_CODEC_CUDA_ERR_UNSUPPORTED_FORMAT:
      return Status::UnsupportedFormat;
    case IMAGE_CODEC_CUDA_ERR_DECODE_FAILED:
      return Status::DecodeFailed;
    case IMAGE_CODEC_CUDA_ERR_ENCODE_FAILED:
      return Status::EncodeFailed;
    case IMAGE_CODEC_CUDA_ERR_IO:
      return Status::Io;
    case IMAGE_CODEC_CUDA_ERR_INTERNAL:
      return Status::Internal;
  }
  return Status::Internal;
}

Error makeError(Status code, const char* stage, image_codec_cuda_status status) {
  return Error{code, std::string("cuda: ") + stage + " failed: " +
                         image_codec_cuda_status_string(status)};
}

int toCudaFormat(Format format) {
  switch (format) {
    case Format::Jpeg:
      return IMAGE_CODEC_CUDA_FORMAT_JPEG;
    case Format::Tiff:
      return IMAGE_CODEC_CUDA_FORMAT_TIFF;
    case Format::Png:
      return IMAGE_CODEC_CUDA_FORMAT_PNG;
    case Format::Unknown:
      break;
  }
  return IMAGE_CODEC_CUDA_FORMAT_UNKNOWN;
}

// offload::Image -> image_codec_cuda_image（malloc，由 image_codec_cuda_image_free 释放）。
// C ABI 没有借用语义，这一步必然复制整幅图。
bool toCApiImage(const Image& src, image_codec_cuda_image* out) {
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

class CudaBackend final : public ICodecBackend {
 public:
  CudaBackend() = default;

  ~CudaBackend() override {
    if (codec_ != nullptr) image_codec_cuda_codec_destroy(codec_);
  }

  BackendKind kind() const override { return BackendKind::Cuda; }

  bool supports(Format format) const override {
    // CUDA 后端三格式齐全（JPEG 走 nvJPEG，TIFF 走 GPU LZW，PNG 走 GPU 行滤波）。
    return format == Format::Jpeg || format == Format::Tiff ||
           format == Format::Png;
  }

  ExecutionMode executionMode(Format format) const override {
    return supports(format) ? ExecutionMode::HybridCpuGpu
                             : ExecutionMode::Unavailable;
  }

  Result<void> available() const override {
    if (available_checked_) return available_result_;

    available_checked_ = true;
    image_codec_cuda_codec* codec = nullptr;
    const image_codec_cuda_status st = image_codec_cuda_codec_create(&codec, 0);
    if (st != IMAGE_CODEC_CUDA_OK) {
      available_result_ = fail<void>(
          Status::BackendUnavailable,
          std::string("cuda: no usable device (") +
              image_codec_cuda_status_string(st) + ")");
      return available_result_;
    }
    codec_ = codec;

    // 问一次设备能力：nvJPEG 缺失时 JPEG 方向必须回退，而不是等编码时
    // 才抛一个看不懂的错误码。查询失败不影响句柄本身可用。
    image_codec_cuda_capabilities caps{};
    if (image_codec_cuda_get_capabilities(codec_, &caps) ==
        IMAGE_CODEC_CUDA_OK) {
      device_name_ = caps.device_name;
      caps_note_ = caps.note;
      jpeg_decode_ = caps.jpeg_decode != 0;
      jpeg_encode_ = caps.jpeg_encode != 0;
      tiff_decode_ = caps.tiff_decode != 0;
      tiff_encode_ = caps.tiff_encode != 0;
      png_decode_ = caps.png_decode != 0;
      png_encode_ = caps.png_encode != 0;
    }

    available_result_ = {};
    return available_result_;
  }

  Result<std::vector<uint8_t>> encode(Format format, const Image& image,
                                      const EncodeParams& params) override {
    if (!supports(format)) {
      return fail<std::vector<uint8_t>>(
          Status::UnsupportedFormat,
          std::string("cuda: format not supported: ") + formatName(format));
    }
    if (auto ok = available(); !ok) return std::unexpected(ok.error());
    if (!directionReady(format, /*encoding=*/true)) {
      return fail<std::vector<uint8_t>>(
          Status::UnsupportedFormat,
          std::string("cuda: encode not available for ") + formatName(format) +
              (caps_note_.empty() ? "" : (": " + caps_note_)));
    }

    image_codec_cuda_image img;
    if (!toCApiImage(image, &img)) {
      return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                        "cuda: image copy failed");
    }

    image_codec_cuda_encode_params cparams{};
    cparams.jpeg_quality = params.jpeg_quality;
    cparams.jpeg_subsampling = params.jpeg_subsampling;
    cparams.tiff_compression = params.tiff_compression;
    cparams.tiff_rows_per_strip = params.tiff_rows_per_strip;
    cparams.tiff_tile_size = params.tiff_tile_size;
    cparams.png_compression = params.png_compression;
    cparams.png_filter = params.png_filter;

    const int fmt = toCudaFormat(format);

    // 先查所需大小再分配（C API 约定：缓冲不足时回填所需长度）。
    //
    // 注意 OUT_OF_MEMORY 在这里有两种含义，必须区分：
    //   * needed > 0 —— 「缓冲不够，大小是这么多」，属于协议的一部分；
    //   * needed == 0 —— 真的资源不足（显存/驱动内部分配失败），例如超大图
    //     走 nvJPEG 4:4:4 编码。此时要如实报 OutOfMemory 让路由层回退到 CPU，
    //     而不是含糊成 EncodeFailed —— 后者按约定是不回退的。
    size_t needed = 0;
    image_codec_cuda_status st = image_codec_cuda_encode(
        codec_, fmt, &img, &cparams, nullptr, &needed, 0);
    if (st == IMAGE_CODEC_CUDA_ERR_OUT_OF_MEMORY && needed == 0) {
      image_codec_cuda_image_free(&img);
      return fail<std::vector<uint8_t>>(
          Status::OutOfMemory,
          "cuda: encode needs more device memory than available");
    }
    if (st != IMAGE_CODEC_CUDA_ERR_OUT_OF_MEMORY &&
        st != IMAGE_CODEC_CUDA_OK) {
      image_codec_cuda_image_free(&img);
      return std::unexpected(makeError(mapStatus(st), "encode", st));
    }
    if (needed == 0) {
      image_codec_cuda_image_free(&img);
      return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                        "cuda: encode returned empty buffer");
    }

    std::vector<uint8_t> out(needed);
    size_t written = needed;
    st = image_codec_cuda_encode(codec_, fmt, &img, &cparams, out.data(),
                                 &written, out.size());
    image_codec_cuda_image_free(&img);

    if (st != IMAGE_CODEC_CUDA_OK) {
      return std::unexpected(makeError(mapStatus(st), "encode", st));
    }
    out.resize(written);
    return out;
  }

  Result<Image> decode(Format format, const uint8_t* data,
                       size_t size) override {
    if (!supports(format)) {
      return fail<Image>(Status::UnsupportedFormat,
                         std::string("cuda: format not supported: ") +
                             formatName(format));
    }
    if (auto ok = available(); !ok) return std::unexpected(ok.error());
    if (!directionReady(format, /*encoding=*/false)) {
      return fail<Image>(Status::UnsupportedFormat,
                         std::string("cuda: decode not available for ") +
                             formatName(format) +
                             (caps_note_.empty() ? "" : (": " + caps_note_)));
    }

    image_codec_cuda_image img{};
    const image_codec_cuda_status st =
        image_codec_cuda_decode(codec_, toCudaFormat(format), data, size, &img);
    if (st != IMAGE_CODEC_CUDA_OK) {
      return std::unexpected(makeError(mapStatus(st), "decode", st));
    }

    Image out;
    out.width = static_cast<int>(img.width);
    out.height = static_cast<int>(img.height);
    out.channels = static_cast<int>(img.channels);
    // 解码器可能给出带 padding 的 stride；offload::Image 是紧凑布局，
    // 必须逐行拷贝而不是整块 assign（否则行宽不一致的图像会错位）。
    const size_t tight = out.rowBytes();
    if (img.stride == tight) {
      out.pixels.assign(img.data, img.data + img.data_size);
    } else {
      out.pixels.resize(tight * static_cast<size_t>(out.height));
      const size_t rows = static_cast<size_t>(out.height);
      const size_t copy = tight < img.stride ? tight : img.stride;
      for (size_t y = 0; y < rows; ++y) {
        std::memcpy(out.pixels.data() + y * tight,
                    img.data + y * img.stride, copy);
      }
    }
    image_codec_cuda_image_free(&img);

    if (!out.valid()) {
      return fail<Image>(Status::DecodeFailed,
                         "cuda: decoded image is inconsistent");
    }
    return out;
  }

  Result<double> mse(const Image& a, const Image& b) override {
    return scalarMetric(a, b, /*which=*/0);
  }

  Result<double> psnr(const Image& a, const Image& b) override {
    return scalarMetric(a, b, /*which=*/1);
  }

  Result<SsimResult> ssim(const Image& a, const Image& b) override {
    if (auto ok = available(); !ok) return std::unexpected(ok.error());
    if (auto ok = validatePair(a, b); !ok) return std::unexpected(ok.error());

    image_codec_cuda_image ia;
    image_codec_cuda_image ib;
    if (!toCApiImage(a, &ia)) {
      return fail<SsimResult>(Status::OutOfMemory, "cuda: image copy failed");
    }
    if (!toCApiImage(b, &ib)) {
      image_codec_cuda_image_free(&ia);
      return fail<SsimResult>(Status::OutOfMemory, "cuda: image copy failed");
    }

    image_codec_cuda_ssim result{};
    const image_codec_cuda_status st =
        image_codec_cuda_metric_ssim(codec_, &ia, &ib, &result);
    image_codec_cuda_image_free(&ia);
    image_codec_cuda_image_free(&ib);
    if (st != IMAGE_CODEC_CUDA_OK) {
      return std::unexpected(makeError(mapStatus(st), "ssim", st));
    }

    SsimResult out;
    out.ssim = result.ssim;
    out.ms_ssim = result.ms_ssim;
    out.levels = result.levels;
    return out;
  }

 private:
  // 能力表按「格式 + 方向」判断：nvJPEG 缺失只影响 JPEG，TIFF/PNG 仍然能跑。
  bool directionReady(Format format, bool encoding) const {
    if (device_name_.empty()) return true;  // 能力查询失败：交给实际调用失败
    switch (format) {
      case Format::Jpeg:
        return encoding ? jpeg_encode_ : jpeg_decode_;
      case Format::Tiff:
        return encoding ? tiff_encode_ : tiff_decode_;
      case Format::Png:
        return encoding ? png_encode_ : png_decode_;
      case Format::Unknown:
        break;
    }
    return false;
  }

  static Result<void> validatePair(const Image& a, const Image& b) {
    if (!a.valid() || !b.valid()) {
      return fail<void>(Status::InvalidArgument, "cuda: invalid image");
    }
    if (a.width != b.width || a.height != b.height ||
        a.channels != b.channels) {
      return fail<void>(Status::InvalidArgument,
                        "cuda: metrics require identical geometry");
    }
    return {};
  }

  Result<double> scalarMetric(const Image& a, const Image& b, int which) {
    if (auto ok = available(); !ok) return std::unexpected(ok.error());
    if (auto ok = validatePair(a, b); !ok) return std::unexpected(ok.error());

    image_codec_cuda_image ia;
    image_codec_cuda_image ib;
    if (!toCApiImage(a, &ia)) {
      return fail<double>(Status::OutOfMemory, "cuda: image copy failed");
    }
    if (!toCApiImage(b, &ib)) {
      image_codec_cuda_image_free(&ia);
      return fail<double>(Status::OutOfMemory, "cuda: image copy failed");
    }

    double value = 0.0;
    const image_codec_cuda_status st =
        which == 0 ? image_codec_cuda_metric_mse(codec_, &ia, &ib, &value)
                   : image_codec_cuda_metric_psnr(codec_, &ia, &ib, &value);
    image_codec_cuda_image_free(&ia);
    image_codec_cuda_image_free(&ib);

    if (st != IMAGE_CODEC_CUDA_OK) {
      return std::unexpected(
          makeError(mapStatus(st), which == 0 ? "mse" : "psnr", st));
    }
    return value;
  }

  mutable bool available_checked_ = false;
  mutable Result<void> available_result_ = {};
  mutable image_codec_cuda_codec* codec_ = nullptr;

  // 能力表在第一次 available()（const）里探测并缓存，因此这些字段是 mutable：
  // 探测是幂等的读操作，不该逼调用方拿非 const 引用。
  mutable std::string device_name_;
  mutable std::string caps_note_;
  mutable bool jpeg_decode_ = false;
  mutable bool jpeg_encode_ = false;
  mutable bool tiff_decode_ = false;
  mutable bool tiff_encode_ = false;
  mutable bool png_decode_ = false;
  mutable bool png_encode_ = false;
};

}  // namespace

std::unique_ptr<ICodecBackend> makeCudaBackend() {
  return std::make_unique<CudaBackend>();
}

}  // namespace offload
}  // namespace image_codec

#else

namespace image_codec {
namespace offload {

std::unique_ptr<ICodecBackend> makeCudaBackend() { return nullptr; }

}  // namespace offload
}  // namespace image_codec

#endif  // IMAGECODEC_OFFLOAD_WITH_CUDA
