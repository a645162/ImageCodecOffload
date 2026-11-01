// File      : vulkan_backend.cpp
// Brief     : ImageCodecVulkan 的 ICodecBackend 适配
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 与 CUDA / OpenCL 适配层同样只做三件事：结构体转换、状态码映射、失败原因补
// 后端名。三种格式都支持，但分工不同（见 docs/README.md 的能力矩阵）：JPEG 的
// Huffman 与容器在 CPU、DCT/颜色变换在 GPU；TIFF 的 LZW 在 CPU、Predictor 在
// GPU；PNG 的行滤波在 GPU、DEFLATE 在 CPU。
#include "vulkan_backend.h"

#ifdef IMAGECODEC_OFFLOAD_WITH_VULKAN

#include <VulkanImageCodec/image_codec_vulkan.h>

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "cpu_backend.h"

namespace image_codec {
namespace offload {

namespace {

Status mapStatus(image_codec_vulkan_status status) {
  switch (status) {
  case IMAGE_CODEC_VULKAN_OK:
    return Status::Ok;
  case IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT:
    return Status::InvalidArgument;
  case IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY:
    return Status::OutOfMemory;
  case IMAGE_CODEC_VULKAN_ERR_BACKEND_UNAVAILABLE:
    return Status::BackendUnavailable;
  case IMAGE_CODEC_VULKAN_ERR_UNSUPPORTED_FORMAT:
    return Status::UnsupportedFormat;
  case IMAGE_CODEC_VULKAN_ERR_DECODE_FAILED:
    return Status::DecodeFailed;
  case IMAGE_CODEC_VULKAN_ERR_ENCODE_FAILED:
    return Status::EncodeFailed;
  case IMAGE_CODEC_VULKAN_ERR_IO:
    return Status::Io;
  case IMAGE_CODEC_VULKAN_ERR_INTERNAL:
    return Status::Internal;
  }
  return Status::Internal;
}

Error makeError(Status code, const char *stage,
                image_codec_vulkan_status status) {
  return Error{code, std::string("vulkan: ") + stage + " failed: " +
                         image_codec_vulkan_status_string(status)};
}

int toVulkanFormat(Format format) {
  switch (format) {
  case Format::Jpeg:
    return IMAGE_CODEC_VULKAN_FORMAT_JPEG;
  case Format::Tiff:
    return IMAGE_CODEC_VULKAN_FORMAT_TIFF;
  case Format::Png:
    return IMAGE_CODEC_VULKAN_FORMAT_PNG;
  case Format::Unknown:
    break;
  }
  return IMAGE_CODEC_VULKAN_FORMAT_UNKNOWN;
}

// offload::Image -> image_codec_vulkan_image（malloc，由
// image_codec_vulkan_image_free 释放）。C ABI 没有借用语义，这一步必然复制。
bool toCApiImage(const Image &src, image_codec_vulkan_image *out) {
  std::memset(out, 0, sizeof(*out));
  out->width = static_cast<uint32_t>(src.width);
  out->height = static_cast<uint32_t>(src.height);
  out->channels = static_cast<uint32_t>(src.channels);
  out->stride = src.rowBytes();
  out->data_size = src.pixels.size();
  out->data = static_cast<uint8_t *>(std::malloc(out->data_size));
  if (out->data == nullptr) return false;
  std::memcpy(out->data, src.pixels.data(), out->data_size);
  return true;
}

Result<Image> fromCApiImage(const image_codec_vulkan_image &src) {
  Image out;
  out.width = static_cast<int>(src.width);
  out.height = static_cast<int>(src.height);
  out.channels = static_cast<int>(src.channels);
  const size_t tight = out.rowBytes();
  if (src.data == nullptr || src.stride < tight) {
    return fail<Image>(Status::DecodeFailed, "vulkan: invalid staged image");
  }
  out.pixels.resize(tight * static_cast<size_t>(out.height));
  for (int y = 0; y < out.height; ++y) {
    std::memcpy(out.pixels.data() + static_cast<size_t>(y) * tight,
                src.data + static_cast<size_t>(y) * src.stride, tight);
  }
  return out;
}

class VulkanBackend final : public ICodecBackend {
public:
  VulkanBackend() = default;

  ~VulkanBackend() override {
    if (codec_ != nullptr) image_codec_vulkan_codec_destroy(codec_);
  }

  BackendKind kind() const override { return BackendKind::Vulkan; }

  bool supports(Format format) const override {
    // 三种格式都支持；通道数（JPEG 的 1/3）由后端 C API 自己回答，
    // 路由层不替它猜——猜错一次就是一次静默降维。
    if (format == Format::Jpeg || format == Format::Tiff ||
        format == Format::Png) {
      return true;
    }
#if defined(IMAGECODEC_OFFLOAD_WITH_OPENJPEG) &&                               \
    defined(IMAGECODEC_OFFLOAD_WITH_JXL)
    return format == Format::Jpeg2000 || format == Format::JpegXl;
#elif defined(IMAGECODEC_OFFLOAD_WITH_OPENJPEG)
    return format == Format::Jpeg2000;
#elif defined(IMAGECODEC_OFFLOAD_WITH_JXL)
    return format == Format::JpegXl;
#else
    return false;
#endif
  }

  bool supports(Format format, CodecDirection direction) const override {
    if (!supports(format)) return false;
    if (format == Format::Jpeg2000 || format == Format::JpegXl) return true;
    if (const auto ready = available(); !ready) return true;
    return directionReady(format, direction == CodecDirection::Encode);
  }

  ExecutionMode executionMode(Format format) const override {
    return supports(format) ? ExecutionMode::HybridCpuGpu
                            : ExecutionMode::Unavailable;
  }

  Result<void> available() const override {
    if (available_checked_) return available_result_;

    available_checked_ = true;
    image_codec_vulkan_codec *codec = nullptr;
    // -1 = 自动选（独显优先）：同一台机器上「跑图形的独显」与「有媒体引擎
    // 的核显」未必是同一块卡，写死 0 会在核显排前的机器上选错。
    const image_codec_vulkan_status st =
        image_codec_vulkan_codec_create(&codec, -1);
    if (st != IMAGE_CODEC_VULKAN_OK) {
      available_result_ =
          fail<void>(Status::BackendUnavailable,
                     std::string("vulkan: no usable device (") +
                         image_codec_vulkan_status_string(st) + ")");
      return available_result_;
    }
    codec_ = codec;

    // 问一次能力表：哪些方向真的能用、以及为什么不能用，都由后端自己说，
    // 不在适配层猜。
    image_codec_vulkan_capabilities caps{};
    if (image_codec_vulkan_get_capabilities(codec_, &caps) ==
        IMAGE_CODEC_VULKAN_OK) {
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

  Result<RuntimeResolutionLimit>
  runtimeResolutionLimit(Format format) const override {
    if (!supports(format)) {
      return fail<RuntimeResolutionLimit>(
          Status::UnsupportedFormat,
          std::string("vulkan: format not supported: ") + formatName(format));
    }
    if (const auto ready = available(); !ready) {
      return std::unexpected(ready.error());
    }
    image_codec_vulkan_memory_info memory{};
    const auto status = image_codec_vulkan_get_memory_info(codec_, &memory);
    if (status != IMAGE_CODEC_VULKAN_OK) {
      return fail<RuntimeResolutionLimit>(
          mapStatus(status), std::string("vulkan: memory query failed: ") +
                                 image_codec_vulkan_status_string(status));
    }
    uint64_t usable = memory.free_bytes;
    if (usable == 0) usable = memory.max_alloc_bytes;
    if (memory.budget_bytes != 0) {
      usable = std::min<uint64_t>(usable, memory.budget_bytes);
    }
    if (memory.max_alloc_bytes != 0) {
      // Each staging buffer must itself fit in one allocation.
      usable = std::min<uint64_t>(usable, memory.max_alloc_bytes * 2);
    }
    constexpr uint32_t kBytesPerPixel = 8;
    return RuntimeResolutionLimit{usable / kBytesPerPixel, usable,
                                  kBytesPerPixel,
                                  "Vulkan free/budget memory; conservative for "
                                  "two 4-channel staging buffers"};
  }

  Result<std::vector<uint8_t>> encode(Format format, const Image &image,
                                      const EncodeParams &params) override {
    if (!supports(format)) {
      return fail<std::vector<uint8_t>>(
          Status::UnsupportedFormat,
          std::string("vulkan: format not supported: ") + formatName(format));
    }
    if (auto ok = available(); !ok) return std::unexpected(ok.error());
    if (format == Format::Jpeg2000 || format == Format::JpegXl) {
      image_codec_vulkan_image input{};
      if (!toCApiImage(image, &input)) {
        return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                          "vulkan: image copy failed");
      }
      image_codec_vulkan_image staged{};
      const auto stage = image_codec_vulkan_gpu_stage(codec_, &input, &staged);
      image_codec_vulkan_image_free(&input);
      if (stage != IMAGE_CODEC_VULKAN_OK) {
        return std::unexpected(makeError(mapStatus(stage), "gpu-stage", stage));
      }
      auto staged_image = fromCApiImage(staged);
      image_codec_vulkan_image_free(&staged);
      if (!staged_image) return std::unexpected(staged_image.error());
      cpu::CpuBackend cpu;
      return cpu.encode(format, *staged_image, params);
    }
    if (!directionReady(format, /*encoding=*/true)) {
      return fail<std::vector<uint8_t>>(
          Status::UnsupportedFormat,
          std::string("vulkan: encode not available for ") +
              formatName(format) +
              (caps_note_.empty() ? "" : (": " + caps_note_)));
    }

    image_codec_vulkan_image img;
    if (!toCApiImage(image, &img)) {
      return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                        "vulkan: image copy failed");
    }

    image_codec_vulkan_encode_params cparams{};
    cparams.jpeg_quality = params.jpeg_quality;
    cparams.jpeg_subsampling = params.jpeg_subsampling;
    cparams.tiff_compression = params.tiff_compression;
    cparams.tiff_rows_per_strip = params.tiff_rows_per_strip;
    cparams.tiff_tile_size = params.tiff_tile_size;
    // 0 = 后端默认（2）；这里显式传 2 而不是 0，避免「默认值」在两处各有一份定义。
    cparams.tiff_predictor = 2;
    cparams.png_compression = params.png_compression;
    cparams.png_filter = params.png_filter;
    cparams.band_rows = 0; // 后端默认（256）

    const int fmt = toVulkanFormat(format);

    // 先查所需大小再分配（C API 约定：缓冲不足时回填所需长度）。
    //
    // OUT_OF_MEMORY 在这里有两种含义，必须区分：
    //   * needed > 0 —— 「缓冲不够，大小是这么多」，属于协议的一部分；
    //   * needed == 0 —— 真的资源不足（显存装不下），如实报 OutOfMemory 让
    //     路由层回退到 CPU，而不是含糊成 EncodeFailed（后者不参与回退）。
    size_t needed = 0;
    image_codec_vulkan_status st = image_codec_vulkan_encode(
        codec_, fmt, &img, &cparams, nullptr, &needed, 0);
    if (st == IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY && needed == 0) {
      image_codec_vulkan_image_free(&img);
      return fail<std::vector<uint8_t>>(
          Status::OutOfMemory,
          "vulkan: encode needs more device memory than available");
    }
    if (st != IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY &&
        st != IMAGE_CODEC_VULKAN_OK) {
      image_codec_vulkan_image_free(&img);
      return std::unexpected(makeError(mapStatus(st), "encode", st));
    }
    if (needed == 0) {
      image_codec_vulkan_image_free(&img);
      return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                        "vulkan: encode returned empty buffer");
    }

    std::vector<uint8_t> out(needed);
    size_t written = needed;
    st = image_codec_vulkan_encode(codec_, fmt, &img, &cparams, out.data(),
                                   &written, out.size());
    image_codec_vulkan_image_free(&img);

    if (st != IMAGE_CODEC_VULKAN_OK) {
      return std::unexpected(makeError(mapStatus(st), "encode", st));
    }
    out.resize(written);
    return out;
  }

  Result<Image> decode(Format format, const uint8_t *data,
                       size_t size) override {
    if (!supports(format)) {
      return fail<Image>(Status::UnsupportedFormat,
                         std::string("vulkan: format not supported: ") +
                             formatName(format));
    }
    if (auto ok = available(); !ok) return std::unexpected(ok.error());
    if (format == Format::Jpeg2000 || format == Format::JpegXl) {
      cpu::CpuBackend cpu;
      auto decoded = cpu.decode(format, data, size);
      if (!decoded) return std::unexpected(decoded.error());
      image_codec_vulkan_image input{};
      if (!toCApiImage(*decoded, &input)) {
        return fail<Image>(Status::OutOfMemory, "vulkan: image copy failed");
      }
      image_codec_vulkan_image staged{};
      const auto stage = image_codec_vulkan_gpu_stage(codec_, &input, &staged);
      image_codec_vulkan_image_free(&input);
      if (stage != IMAGE_CODEC_VULKAN_OK) {
        return std::unexpected(makeError(mapStatus(stage), "gpu-stage", stage));
      }
      auto result = fromCApiImage(staged);
      image_codec_vulkan_image_free(&staged);
      return result;
    }
    if (!directionReady(format, /*encoding=*/false)) {
      return fail<Image>(Status::UnsupportedFormat,
                         std::string("vulkan: decode not available for ") +
                             formatName(format) +
                             (caps_note_.empty() ? "" : (": " + caps_note_)));
    }

    image_codec_vulkan_image img{};
    const image_codec_vulkan_status st = image_codec_vulkan_decode(
        codec_, toVulkanFormat(format), data, size, &img);
    if (st != IMAGE_CODEC_VULKAN_OK) {
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
        std::memcpy(out.pixels.data() + y * tight, img.data + y * img.stride,
                    copy);
      }
    }
    image_codec_vulkan_image_free(&img);

    if (!out.valid()) {
      return fail<Image>(Status::DecodeFailed,
                         "vulkan: decoded image is inconsistent");
    }
    return out;
  }

  Result<double> mse(const Image &a, const Image &b) override {
    return scalarMetric(a, b, /*which=*/0);
  }

  Result<double> psnr(const Image &a, const Image &b) override {
    return scalarMetric(a, b, /*which=*/1);
  }

  Result<SsimResult> ssim(const Image &a, const Image &b) override {
    if (auto ok = available(); !ok) return std::unexpected(ok.error());
    if (auto ok = validatePair(a, b); !ok) return std::unexpected(ok.error());

    image_codec_vulkan_image ia;
    image_codec_vulkan_image ib;
    if (!toCApiImage(a, &ia)) {
      return fail<SsimResult>(Status::OutOfMemory, "vulkan: image copy failed");
    }
    if (!toCApiImage(b, &ib)) {
      image_codec_vulkan_image_free(&ia);
      return fail<SsimResult>(Status::OutOfMemory, "vulkan: image copy failed");
    }

    image_codec_vulkan_ssim result{};
    const image_codec_vulkan_status st =
        image_codec_vulkan_metric_ssim(codec_, &ia, &ib, &result);
    image_codec_vulkan_image_free(&ia);
    image_codec_vulkan_image_free(&ib);
    if (st != IMAGE_CODEC_VULKAN_OK) {
      return std::unexpected(makeError(mapStatus(st), "ssim", st));
    }

    SsimResult out;
    out.ssim = result.ssim;
    out.ms_ssim = result.ms_ssim;
    out.levels = result.levels;
    return out;
  }

private:
  // 能力表按「格式 + 方向」判断：某个方向缺失只影响那一个方向。
  bool directionReady(Format format, bool encoding) const {
    if (device_name_.empty()) return true; // 能力查询失败：交给实际调用失败
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

  static Result<void> validatePair(const Image &a, const Image &b) {
    if (!a.valid() || !b.valid()) {
      return fail<void>(Status::InvalidArgument, "vulkan: invalid image");
    }
    if (a.width != b.width || a.height != b.height ||
        a.channels != b.channels) {
      return fail<void>(Status::InvalidArgument,
                        "vulkan: metrics require identical geometry");
    }
    return {};
  }

  Result<double> scalarMetric(const Image &a, const Image &b, int which) {
    if (auto ok = available(); !ok) return std::unexpected(ok.error());
    if (auto ok = validatePair(a, b); !ok) return std::unexpected(ok.error());

    image_codec_vulkan_image ia;
    image_codec_vulkan_image ib;
    if (!toCApiImage(a, &ia)) {
      return fail<double>(Status::OutOfMemory, "vulkan: image copy failed");
    }
    if (!toCApiImage(b, &ib)) {
      image_codec_vulkan_image_free(&ia);
      return fail<double>(Status::OutOfMemory, "vulkan: image copy failed");
    }

    double value = 0.0;
    const image_codec_vulkan_status st =
        which == 0 ? image_codec_vulkan_metric_mse(codec_, &ia, &ib, &value)
                   : image_codec_vulkan_metric_psnr(codec_, &ia, &ib, &value);
    image_codec_vulkan_image_free(&ia);
    image_codec_vulkan_image_free(&ib);

    if (st != IMAGE_CODEC_VULKAN_OK) {
      return std::unexpected(
          makeError(mapStatus(st), which == 0 ? "mse" : "psnr", st));
    }
    return value;
  }

  mutable bool available_checked_ = false;
  mutable Result<void> available_result_ = {};
  mutable image_codec_vulkan_codec *codec_ = nullptr;

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

} // namespace

std::unique_ptr<ICodecBackend> makeVulkanBackend() {
  return std::make_unique<VulkanBackend>();
}

} // namespace offload
} // namespace image_codec

#else

namespace image_codec {
namespace offload {

std::unique_ptr<ICodecBackend> makeVulkanBackend() { return nullptr; }

} // namespace offload
} // namespace image_codec

#endif // IMAGECODEC_OFFLOAD_WITH_VULKAN
