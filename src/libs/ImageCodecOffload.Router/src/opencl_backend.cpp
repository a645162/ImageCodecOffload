// opencl_backend.cpp - OpenCLImageCodec 的 ICodecBackend 适配。
#include "opencl_backend.h"

#ifdef IMAGECODEC_OFFLOAD_WITH_OPENCL

#include <OpenCLImageCodec/image_codec.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "container_probe.h"

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

Error makeError(Status code, const char* stage, image_codec_status status) {
  return Error{code, std::string("opencl: ") + stage + " failed: " +
                         image_codec_status_string(status)};
}

image_codec_format toOpenClFormat(Format format) {
  return format == Format::Jpeg ? IMAGE_CODEC_FORMAT_JPEG
                                : IMAGE_CODEC_FORMAT_TIFF;
}

// offload::Image -> image_codec_image（malloc，由调用方 free）。
// 这一步必然复制整幅图：C ABI 的 image 是裸指针 + 长度，没有借用语义。
bool toCApiImage(const Image& src, image_codec_image* out) {
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

class OpenClBackend final : public ICodecBackend {
 public:
  OpenClBackend() = default;

  ~OpenClBackend() override {
    if (codec_ != nullptr) image_codec_codec_destroy(codec_);
  }

  BackendKind kind() const override { return BackendKind::OpenCl; }

  bool supports(Format format) const override {
    // OpenCL 后端目前只有 JPEG 与 TIFF；PNG 由 CPU / CUDA 承担。
    return format == Format::Jpeg || format == Format::Tiff;
  }

  Result<void> available() const override {
    if (available_checked_) return available_result_;

    available_checked_ = true;
    image_codec_codec* codec = nullptr;
    const image_codec_status st =
        image_codec_codec_create(&codec, IMAGE_CODEC_BACKEND_OPENCL);
    if (st != IMAGE_CODEC_OK) {
      available_result_ = fail<void>(
          Status::BackendUnavailable,
          std::string("opencl: no usable device (") +
              image_codec_status_string(st) + ")");
      return available_result_;
    }
    codec_ = codec;
    available_result_ = {};
    return available_result_;
  }

  Result<std::vector<uint8_t>> encode(Format format, const Image& image,
                                      const EncodeParams& params) override {
    if (!supports(format)) {
      return fail<std::vector<uint8_t>>(
          Status::UnsupportedFormat,
          std::string("opencl: format not supported: ") + formatName(format));
    }
    if (auto ok = available(); !ok) {
      return std::unexpected(ok.error());
    }
    // OpenCL 后端的 C ABI 只定义 24-bit RGB（stride = width * 3）。灰度图必须
    // 回退到 CPU/CUDA，而不是在这里悄悄复制成三份——那会让「1 通道进、3 通道出」
    // 变成一个没人解释得清的体积膨胀。
    if (image.channels != 3) {
      return fail<std::vector<uint8_t>>(
          Status::UnsupportedFormat,
          "opencl: only 3-channel images are supported");
    }

    image_codec_image img;
    if (!toCApiImage(image, &img)) {
      return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                        "opencl: image copy failed");
    }

    image_codec_encode_params cparams{};
    cparams.jpeg_quality = params.jpeg_quality;
    cparams.tiff_compression = params.tiff_compression;
    cparams.tiff_rows_per_strip = params.tiff_rows_per_strip;

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

      capacity = written;  // C API 回填了所需大小
      out.resize(capacity);
    }

    image_codec_image_free(&img);

    if (!done) {
      return std::unexpected(makeError(Status::EncodeFailed, "encode", st));
    }
    out.resize(written);
    return out;
  }

  Result<Image> decode(Format format, const uint8_t* data,
                       size_t size) override {
    if (!supports(format)) {
      return fail<Image>(Status::UnsupportedFormat,
                         std::string("opencl: format not supported: ") +
                             formatName(format));
    }
    if (auto ok = available(); !ok) return std::unexpected(ok.error());

    // OpenCL 后端的 C ABI 只有 24-bit RGB 一种形态，**解码方向也一样**：
    // 一张灰度 JPEG 交进去会被悄悄升维成 3 通道，灰度 TIFF 则直接失败。
    // 后者尤其糟——它会以 DecodeFailed 返回，而 DecodeFailed 按约定是不回退的，
    // 于是「自动」模式在一张完全正常的灰度 TIFF 上直接报错。
    // 所以先只读头问一次「这张图本来几通道」，表示不了就明确说不支持，
    // 让路由层回退到 CPU（CPU 保留 1 通道）。
    if (auto channels = cpu::probeChannels(format, data, size); channels) {
      if (*channels != 3) {
        return fail<Image>(Status::UnsupportedFormat,
                           "opencl: only 3-channel images are supported");
      }
    }

    image_codec_image img{};
    const image_codec_status st = image_codec_codec_decode(
        codec_, toOpenClFormat(format), data, size, &img);
    if (st != IMAGE_CODEC_OK) {
      return std::unexpected(makeError(Status::DecodeFailed, "decode", st));
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

  Result<double> mse(const Image& a, const Image& b) override {
    return dualMetric(a, b, /*which=*/0);
  }

  Result<double> psnr(const Image& a, const Image& b) override {
    return dualMetric(a, b, /*which=*/1);
  }

  Result<SsimResult> ssim(const Image& a, const Image& b) override {
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
  static Result<void> validatePair(const Image& a, const Image& b) {
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

  Result<double> dualMetric(const Image& a, const Image& b, int which) {
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
      return std::unexpected(makeError(Status::Internal, which == 0 ? "mse" : "psnr", st));
    }
    return value;
  }

  mutable bool available_checked_ = false;
  mutable Result<void> available_result_ = {};
  mutable image_codec_codec* codec_ = nullptr;
};

}  // namespace

std::unique_ptr<ICodecBackend> makeOpenClBackend() {
  return std::make_unique<OpenClBackend>();
}

}  // namespace offload
}  // namespace image_codec

#else

namespace image_codec {
namespace offload {

std::unique_ptr<ICodecBackend> makeOpenClBackend() { return nullptr; }

}  // namespace offload
}  // namespace image_codec

#endif  // IMAGECODEC_OFFLOAD_WITH_OPENCL
