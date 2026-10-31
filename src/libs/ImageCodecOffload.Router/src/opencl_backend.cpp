/*
 * File: opencl_backend.cpp
 * Brief: Opencl backend.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// opencl_backend.cpp - OpenCLImageCodec 的 ICodecBackend 适配。
#include "opencl_backend.h"

#ifdef IMAGECODEC_OFFLOAD_WITH_OPENCL

#include <OpenCLImageCodec/image_codec.h>
#include <CL/cl.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "cpu_backend.h"

namespace image_codec {
namespace offload {

namespace {

Status mapStatus(image_codec_status status) {
  switch (status) {
  case IMAGE_CODEC_OK:
    return Status::Ok;
  case IMAGE_CODEC_ERR_INVALID_ARGUMENT:
    return Status::InvalidArgument;
  case IMAGE_CODEC_ERR_OUT_OF_MEMORY:
    return Status::OutOfMemory;
  case IMAGE_CODEC_ERR_BACKEND_UNAVAILABLE:
    return Status::BackendUnavailable;
  case IMAGE_CODEC_ERR_UNSUPPORTED_FORMAT:
    return Status::UnsupportedFormat;
  case IMAGE_CODEC_ERR_DECODE_FAILED:
    return Status::DecodeFailed;
  case IMAGE_CODEC_ERR_ENCODE_FAILED:
    return Status::EncodeFailed;
  case IMAGE_CODEC_ERR_IO:
    return Status::Io;
  case IMAGE_CODEC_ERR_INTERNAL:
    return Status::Internal;
  }
  return Status::Internal;
}

Error makeError(Status code, const char *stage, image_codec_status status) {
  return Error{code, std::string("opencl: ") + stage +
                         " failed: " + image_codec_status_string(status)};
}

image_codec_format toOpenClFormat(Format format) {
  switch (format) {
  case Format::Jpeg:
    return IMAGE_CODEC_FORMAT_JPEG;
  case Format::Tiff:
    return IMAGE_CODEC_FORMAT_TIFF;
  case Format::Png:
    return IMAGE_CODEC_FORMAT_PNG;
  }
  return IMAGE_CODEC_FORMAT_JPEG;
}

// offload::Image -> image_codec_image（malloc，由调用方 free）。
// 这一步必然复制整幅图：C ABI 的 image 是裸指针 + 长度，没有借用语义。
bool toCApiImage(const Image &src, image_codec_image *out) {
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

Result<Image> fromCApiImage(const image_codec_image &src) {
  Image out;
  out.width = static_cast<int>(src.width);
  out.height = static_cast<int>(src.height);
  out.channels = static_cast<int>(src.channels);
  const size_t tight = out.rowBytes();
  if (src.data == nullptr || src.stride < tight) {
    return fail<Image>(Status::DecodeFailed, "opencl: invalid staged image");
  }
  out.pixels.resize(tight * static_cast<size_t>(out.height));
  for (int y = 0; y < out.height; ++y) {
    std::memcpy(out.pixels.data() + static_cast<size_t>(y) * tight,
                src.data + static_cast<size_t>(y) * src.stride, tight);
  }
  return out;
}

class OpenClBackend final : public ICodecBackend {
public:
  OpenClBackend() = default;

  ~OpenClBackend() override {
    if (codec_ != nullptr) image_codec_codec_destroy(codec_);
  }

  BackendKind kind() const override { return BackendKind::OpenCl; }

  bool supports(Format format) const override {
    // TIFF / PNG / JPEG 三个格式都有 GPU 参与的重活：TIFF=LZW 编码、PNG=行滤波、
    // JPEG=DCT/颜色变换。容器与熵编码仍留在 CPU（见 docs/README.md 能力矩阵）。
    if (format == Format::Jpeg || format == Format::Tiff ||
        format == Format::Png) {
      return true;
    }
#if defined(IMAGECODEC_OFFLOAD_WITH_OPENJPEG) && defined(IMAGECODEC_OFFLOAD_WITH_JXL)
    return format == Format::Jpeg2000 || format == Format::JpegXl;
#elif defined(IMAGECODEC_OFFLOAD_WITH_OPENJPEG)
    return format == Format::Jpeg2000;
#elif defined(IMAGECODEC_OFFLOAD_WITH_JXL)
    return format == Format::JpegXl;
#else
    return false;
#endif
  }

  ExecutionMode executionMode(Format format) const override {
    return supports(format) ? ExecutionMode::HybridCpuGpu
                             : ExecutionMode::Unavailable;
  }

  Result<void> available() const override {
    if (available_checked_) return available_result_;

    available_checked_ = true;
    image_codec_codec *codec = nullptr;
    const image_codec_status st =
        image_codec_codec_create(&codec, IMAGE_CODEC_BACKEND_OPENCL);
    if (st != IMAGE_CODEC_OK) {
      available_result_ = fail<void>(Status::BackendUnavailable,
                                     std::string("opencl: no usable device (") +
                                         image_codec_status_string(st) + ")");
      return available_result_;
    }
    codec_ = codec;
    available_result_ = {};
    return available_result_;
  }

  Result<RuntimeResolutionLimit>
  runtimeResolutionLimit(Format format) const override {
    if (!supports(format)) {
      return fail<RuntimeResolutionLimit>(
          Status::UnsupportedFormat,
          std::string("opencl: format not supported: ") + formatName(format));
    }
    if (const auto ready = available(); !ready) {
      return std::unexpected(ready.error());
    }

    // OpenCLImageCodec uses device 0 from its shared OclDevice enumeration.
    // Ask that exact first device for the two hard limits that constrain a
    // whole-image staging round-trip: total global memory and one allocation.
    cl_uint platform_count = 0;
    if (clGetPlatformIDs(0, nullptr, &platform_count) != CL_SUCCESS ||
        platform_count == 0) {
      return fail<RuntimeResolutionLimit>(Status::BackendUnavailable,
                                           "opencl: no platform for memory query");
    }
    std::vector<cl_platform_id> platforms(platform_count);
    if (clGetPlatformIDs(platform_count, platforms.data(), nullptr) != CL_SUCCESS) {
      return fail<RuntimeResolutionLimit>(Status::BackendUnavailable,
                                           "opencl: platform enumeration failed");
    }
    cl_device_id device = nullptr;
    for (const cl_platform_id platform : platforms) {
      cl_uint device_count = 0;
      if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr,
                         &device_count) != CL_SUCCESS || device_count == 0) {
        continue;
      }
      std::vector<cl_device_id> devices(device_count);
      if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, device_count,
                         devices.data(), nullptr) == CL_SUCCESS) {
        device = devices.front();
        break;
      }
    }
    if (device == nullptr) {
      return fail<RuntimeResolutionLimit>(Status::BackendUnavailable,
                                           "opencl: no device for memory query");
    }
    cl_ulong global_memory = 0;
    cl_ulong max_allocation = 0;
    if (clGetDeviceInfo(device, CL_DEVICE_GLOBAL_MEM_SIZE,
                        sizeof(global_memory), &global_memory, nullptr) != CL_SUCCESS ||
        clGetDeviceInfo(device, CL_DEVICE_MAX_MEM_ALLOC_SIZE,
                        sizeof(max_allocation), &max_allocation, nullptr) != CL_SUCCESS) {
      return fail<RuntimeResolutionLimit>(Status::BackendUnavailable,
                                           "opencl: device memory query failed");
    }
    constexpr uint32_t kBytesPerPixel = 8;
    const uint64_t usable = std::min<uint64_t>(
        static_cast<uint64_t>(global_memory),
        static_cast<uint64_t>(max_allocation) * 2);
    return RuntimeResolutionLimit{
        usable / kBytesPerPixel, usable, kBytesPerPixel,
        "OpenCL global/max-allocation memory; conservative for two 4-channel staging buffers"};
  }

  Result<std::vector<uint8_t>> encode(Format format, const Image &image,
                                      const EncodeParams &params) override {
    if (!supports(format)) {
      return fail<std::vector<uint8_t>>(
          Status::UnsupportedFormat,
          std::string("opencl: format not supported: ") + formatName(format));
    }
    // OpenCL 的 TIFF 路径目前只实现 NONE(1) 与 LZW(5)。把其它压缩方式
    // 明确归类为「该后端不支持」，让 Router 自动模式回退 CPU；不能把它
    // 继续交给 C API 再得到 invalid-argument，否则回退边界会被破坏。
    if (format == Format::Tiff && params.tiff_compression != 1 &&
        params.tiff_compression != 5) {
      return fail<std::vector<uint8_t>>(
          Status::UnsupportedFormat,
          "opencl: tiff compression is limited to none or lzw");
    }
    if (auto ok = available(); !ok) {
      return std::unexpected(ok.error());
    }

    if (format == Format::Jpeg2000 || format == Format::JpegXl) {
      image_codec_image input{};
      if (!toCApiImage(image, &input)) {
        return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                          "opencl: image copy failed");
      }
      image_codec_image staged{};
      const image_codec_status stage =
          image_codec_codec_gpu_stage(codec_, &input, &staged);
      image_codec_image_free(&input);
      if (stage != IMAGE_CODEC_OK) {
        return std::unexpected(makeError(mapStatus(stage), "gpu-stage", stage));
      }
      auto staged_image = fromCApiImage(staged);
      image_codec_image_free(&staged);
      if (!staged_image) return std::unexpected(staged_image.error());
      cpu::CpuBackend cpu;
      return cpu.encode(format, *staged_image, params);
    }
    // 通道保真：1..4 都原样交给后端，后端不会再升维或丢 Alpha。
    // 具体格式能不能表示某种通道数（例如 JPEG 没有 Alpha）由后端自己回答，
    // 路由层不再替它猜——猜错一次就是一个「静默降维」。
    if (image.channels < 1 || image.channels > 4) {
      return fail<std::vector<uint8_t>>(
          Status::UnsupportedFormat,
          "opencl: only 1- to 4-channel images are supported");
    }

    image_codec_image img;
    if (!toCApiImage(image, &img)) {
      return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                        "opencl: image copy failed");
    }

    image_codec_encode_params cparams{};
    cparams.jpeg_quality = params.jpeg_quality;
    cparams.jpeg_subsampling = params.jpeg_subsampling;
    cparams.tiff_compression = params.tiff_compression;
    cparams.tiff_rows_per_strip = params.tiff_rows_per_strip;
    cparams.png_compression = params.png_compression;
    cparams.png_filter = params.png_filter;

    const image_codec_format fmt = toOpenClFormat(format);

    // OpenCL 后端的 encode 要求 out 非空（空指针被当成参数错误），而 CUDA 后端
    // 允许先传 nullptr 查大小。为了同时适配两者，这里走「估算 → 不足则按回填
    // 长度重试」：两种后端都能用，且不需要后端各自的 ifdef。
    //
    // 起点用源图像素大小：任何可用编码器在有损/无损下都不会显著超过它
    // （LZW/DEFLATE 对不可压数据最多略增，PNG 的 DEFLATE 最坏每 64KB 溢出 5 字节）。
    size_t capacity = image.pixels.size() + image.pixels.size() / 32 + 4096;
    std::vector<uint8_t> out(capacity);

    image_codec_status st = IMAGE_CODEC_OK;
    size_t written = 0;
    bool done = false;
    for (int attempt = 0; attempt < 4 && !done; ++attempt) {
      written = capacity;
      st = image_codec_codec_encode(codec_, fmt, &img, &cparams, out.data(),
                                    &written, out.size());
      if (st == IMAGE_CODEC_OK) {
        done = true;
        break;
      }
      if (st != IMAGE_CODEC_ERR_OUT_OF_MEMORY || written <= capacity) break;

      capacity = written; // C API 回填了所需大小
      out.resize(capacity);
    }

    image_codec_image_free(&img);

    if (!done) {
      return std::unexpected(makeError(mapStatus(st), "encode", st));
    }
    out.resize(written);
    return out;
  }

  Result<Image> decode(Format format, const uint8_t *data,
                       size_t size) override {
    if (!supports(format)) {
      return fail<Image>(Status::UnsupportedFormat,
                         std::string("opencl: format not supported: ") +
                             formatName(format));
    }
    if (auto ok = available(); !ok) return std::unexpected(ok.error());

    if (format == Format::Jpeg2000 || format == Format::JpegXl) {
      cpu::CpuBackend cpu;
      auto decoded = cpu.decode(format, data, size);
      if (!decoded) return std::unexpected(decoded.error());
      image_codec_image input{};
      if (!toCApiImage(*decoded, &input)) {
        return fail<Image>(Status::OutOfMemory, "opencl: image copy failed");
      }
      image_codec_image staged{};
      const image_codec_status stage =
          image_codec_codec_gpu_stage(codec_, &input, &staged);
      image_codec_image_free(&input);
      if (stage != IMAGE_CODEC_OK) {
        return std::unexpected(makeError(mapStatus(stage), "gpu-stage", stage));
      }
      auto result = fromCApiImage(staged);
      image_codec_image_free(&staged);
      return result;
    }

    image_codec_image img{};
    const image_codec_status st = image_codec_codec_decode(
        codec_, toOpenClFormat(format), data, size, &img);
    if (st != IMAGE_CODEC_OK) {
      return std::unexpected(makeError(mapStatus(st), "decode", st));
    }

    Image out;
    out.width = static_cast<int>(img.width);
    out.height = static_cast<int>(img.height);
    out.channels = static_cast<int>(img.channels);
    out.pixels.assign(img.data, img.data + img.data_size);
    image_codec_image_free(&img);

    if (!out.valid()) {
      return fail<Image>(Status::DecodeFailed,
                         "opencl: decoded image is inconsistent");
    }
    return out;
  }

  Result<double> mse(const Image &a, const Image &b) override {
    return dualMetric(a, b, /*which=*/0);
  }

  Result<double> psnr(const Image &a, const Image &b) override {
    return dualMetric(a, b, /*which=*/1);
  }

  Result<SsimResult> ssim(const Image &a, const Image &b) override {
    if (auto ok = available(); !ok) return std::unexpected(ok.error());
    if (auto ok = validatePair(a, b); !ok) return std::unexpected(ok.error());

    image_codec_image ia;
    image_codec_image ib;
    if (!toCApiImage(a, &ia)) {
      return fail<SsimResult>(Status::OutOfMemory, "opencl: image copy failed");
    }
    if (!toCApiImage(b, &ib)) {
      image_codec_image_free(&ia);
      return fail<SsimResult>(Status::OutOfMemory, "opencl: image copy failed");
    }

    image_codec_ssim result{};
    const image_codec_status st =
        image_codec_metric_ssim(codec_, &ia, &ib, &result);
    image_codec_image_free(&ia);
    image_codec_image_free(&ib);
    if (st != IMAGE_CODEC_OK) {
      return std::unexpected(makeError(Status::Internal, "ssim", st));
    }

    SsimResult out;
    out.ssim = result.ssim;
    out.ms_ssim = result.ms_ssim;
    out.levels = result.levels;
    return out;
  }

private:
  static Result<void> validatePair(const Image &a, const Image &b) {
    if (!a.valid() || !b.valid()) {
      return fail<void>(Status::InvalidArgument, "opencl: invalid image");
    }
    if (a.width != b.width || a.height != b.height ||
        a.channels != b.channels) {
      return fail<void>(Status::InvalidArgument,
                        "opencl: metrics require identical geometry");
    }
    return {};
  }

  Result<double> dualMetric(const Image &a, const Image &b, int which) {
    if (auto ok = available(); !ok) return std::unexpected(ok.error());
    if (auto ok = validatePair(a, b); !ok) return std::unexpected(ok.error());

    image_codec_image ia;
    image_codec_image ib;
    if (!toCApiImage(a, &ia)) {
      return fail<double>(Status::OutOfMemory, "opencl: image copy failed");
    }
    if (!toCApiImage(b, &ib)) {
      image_codec_image_free(&ia);
      return fail<double>(Status::OutOfMemory, "opencl: image copy failed");
    }

    double value = 0.0;
    const image_codec_status st =
        which == 0 ? image_codec_metric_mse(codec_, &ia, &ib, &value)
                   : image_codec_metric_psnr(codec_, &ia, &ib, &value);
    image_codec_image_free(&ia);
    image_codec_image_free(&ib);

    if (st != IMAGE_CODEC_OK) {
      return std::unexpected(
          makeError(Status::Internal, which == 0 ? "mse" : "psnr", st));
    }
    return value;
  }

  mutable bool available_checked_ = false;
  mutable Result<void> available_result_ = {};
  mutable image_codec_codec *codec_ = nullptr;
};

} // namespace

std::unique_ptr<ICodecBackend> makeOpenClBackend() {
  return std::make_unique<OpenClBackend>();
}

} // namespace offload
} // namespace image_codec

#else

namespace image_codec {
namespace offload {

std::unique_ptr<ICodecBackend> makeOpenClBackend() { return nullptr; }

} // namespace offload
} // namespace image_codec

#endif // IMAGECODEC_OFFLOAD_WITH_OPENCL
