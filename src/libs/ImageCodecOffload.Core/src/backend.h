// backend.h - 编解码后端接口（CPU / OpenCL / CUDA / Vulkan 各自实现）。
//
// 接口刻意「粗粒度」：一次调用编一整个内存 buffer，而不是暴露窗口/缓冲句柄。
// 上层（ImageViewer 的视口管线、批处理工具）真正需要的是「把这堆像素变成
// TIFF 字节」，细粒度接口只会让每个后端各自发明一套生命周期。
//
// 所有方法都必须能**在不抛异常的情况下报告失败原因**：异常不允许穿过
// 后端 C API 边界（子项目是纯 C ABI），这里是同一个纪律的 C++ 侧体现。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "image_codec_types.h"

namespace image_codec {
namespace offload {

class ICodecBackend {
public:
  virtual ~ICodecBackend() = default;

  virtual BackendKind kind() const = 0;

  // 后端是否可用。返回失败时 message 必须写清具体原因
  // （「没有可用设备」「本构建未包含该后端」「驱动不支持 fp64」...），
  // 上层据此回退并记录，绝不允许只看到「回退到 CPU」而说不出为什么。
  virtual Result<void> available() const = 0;

  // 该后端是否支持某个容器格式。能力是**逐格式**的，不是逐后端：
  // 后端之间的覆盖范围并不天然相同（例如 Vulkan 尚未实现某个格式，或某个
  // 后端只有解码没有编码），路由层必须先问再选，否则「回退」会变成
  // 「选了一个根本不会这格式的后端再失败」。
  virtual bool supports(Format format) const = 0;

  virtual Result<std::vector<uint8_t>> encode(Format format, const Image &image,
                                              const EncodeParams &params) = 0;

  virtual Result<Image> decode(Format format, const uint8_t *data,
                               size_t size) = 0;

  // ---- 画质指标 ----
  // 三个入口独立，理由见 Core/metrics.h。定义必须与其它后端逐位一致。
  virtual Result<double> mse(const Image &a, const Image &b) = 0;
  virtual Result<double> psnr(const Image &a, const Image &b) = 0;
  virtual Result<SsimResult> ssim(const Image &a, const Image &b) = 0;
};

} // namespace offload
} // namespace image_codec
