// cpu_backend.h - CPU 后端（libtiff / libpng / libjpeg-turbo）。
//
// 它同时承担两个角色：没有可用 GPU 时的兜底，以及 GPU 后端的像素级参照。
// 因此它必须永远可用——`available()` 不依赖任何运行期探测。
#pragma once

#include "backend.h"

namespace image_codec {
namespace offload {
namespace cpu {

class CpuBackend final : public ICodecBackend {
 public:
  BackendKind kind() const override { return BackendKind::Cpu; }

  Result<void> available() const override { return {}; }

  bool supports(Format format) const override {
    return format == Format::Jpeg || format == Format::Tiff ||
           format == Format::Png;
  }

  Result<std::vector<uint8_t>> encode(Format format, const Image& image,
                                      const EncodeParams& params) override;

  // CPU 基线是「参数语义的参照实现」：所有进阶参数都在这里落地，GPU 后端
  // 逐项跟它对齐（或明确声明做不到）。因此这里恒为全支持——唯一例外是
  // libtiff 自身没有编入 JPEG 编解码时，TIFF 的 JPEG 压缩做不到。
  EncodeCapabilities encodeCapabilities() const override;

  Result<Image> decode(Format format, const uint8_t* data,
                       size_t size) override;

  Result<double> mse(const Image& a, const Image& b) override;
  Result<double> psnr(const Image& a, const Image& b) override;
  Result<SsimResult> ssim(const Image& a, const Image& b) override;
};

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
