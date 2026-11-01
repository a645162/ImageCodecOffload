/*
 * File: router.h
 * Brief: Router.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// router.h - 后端选择与回退（ImageCodecOffload 的核心价值）。
//
// 上层只调用 Router，不直接碰任何一个后端：它不知道这台机器有 OpenCL 还是有
// CUDA，也不需要知道某个格式在当前后端能不能用。Router 负责：
//   1. 按优先级顺序挑第一个**可用且支持该格式**的后端；
//   2. 该后端明确说「我不行」（不可用 / 不支持 / 显存不够）时依次回退；
//   3. 把每一步的**具体原因**记进 Outcome，而不是只留下一句「用了 CPU」。
//
// 回退边界是刻意收窄的（见 router.cpp 的 retryable）：只有「后端不可用 /
// 不支持该格式 / 资源不足」算「这个后端不适合这一单」。
// 「解码失败 / 编码失败 / 内部错误」属于「这个后端做砸了」——它们不拦路
// （一张 1.9 GB 的图不该因为 OpenCL 分配不出设备缓冲就连 CPU 都不试），但一定
// 记进 notes，并在所有后端都失败时作为最终报错返回，不允许被悄悄救回来。
// 只有「参数非法」会立刻中止：那是调用方的 bug，换后端也一样错。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "backend.h"

namespace image_codec {
namespace offload {

// 一次调用的结果说明：实际用了哪个后端，以及为什么没用首选。
//
// 为什么不用返回值 + 全局日志：上面（ImageViewer 的视口管线、批处理工具）
// 需要把「本次回退原因」显示给用户或写进报告。塞进日志就等于丢失。
struct Outcome {
  BackendKind used = BackendKind::Cpu;
  // 依尝试顺序记录的跳过/回退原因，例如
  // ["cuda: no usable device (backend unavailable)",
  //  "opencl: format not supported: png"]
  std::vector<std::string> notes;

  bool fellBack() const { return !notes.empty(); }
};

class Router {
public:
  // 注册所有被编译进来的后端（CPU 永远有），默认顺序来自
  // defaultOrder()。构造函数不探测设备：探测发生在第一次真正调用时。
  Router();

  ~Router();
  Router(const Router &) = delete;
  Router &operator=(const Router &) = delete;

  // 计算后端优先级（异构计算页可以改成别的顺序）。未知/未编译进来的项被忽略；
  // CPU 永远排在最后，避免配置错误把兜底也弄丢。
  void setOrder(std::vector<BackendKind> order);
  std::vector<BackendKind> order() const;

  // 当前注册进来的后端（含 CPU）。
  std::vector<BackendKind> availableBackends() const;

  // 查询某个已构建后端是否声明支持指定格式；不触发设备探测。
  bool supports(BackendKind kind, Format format) const;
  bool supports(BackendKind kind, Format format,
                CodecDirection direction) const;

  // 返回后端对格式的实际执行模式，不触发设备探测；设备原因由
  // probeBackends() 提供。
  ExecutionMode executionMode(BackendKind kind, Format format) const;
  ExecutionMode executionMode(BackendKind kind, Format format,
                              CodecDirection direction) const;

  // 当前机器上某个后端的保守整图像素上限。该查询允许初始化对应设备，
  // 因而只应由诊断/能力页或真正即将调度的任务调用，不能成为默认编码路径
  // 的隐式副作用。
  Result<RuntimeResolutionLimit> runtimeResolutionLimit(BackendKind kind,
                                                        Format format) const;

  // 每个后端的可用性（含具体原因），供 CLI / 设置页展示。
  // 不做缓存：用户可能在两次调用之间装了驱动、退出了占显存的程序。
  std::vector<std::pair<BackendKind, std::string>> probeBackends() const;

  // ---- 自动选择 + 回退 ----
  Result<std::vector<uint8_t>> encode(Format format, const Image &image,
                                      const EncodeParams &params,
                                      Outcome *outcome = nullptr);

  // 把 TIFF 的一段行独立压缩成一条 strip 的压缩流（分片并行用）。
  // 只考虑声明了 `tiff_strip_sharding` 的后端：其它后端做不到「与整图编码
  // 逐字节相同的片」。
  Result<std::vector<uint8_t>> encodeTiffStrip(const Image &image,
                                               int first_row, int rows,
                                               const EncodeParams &params,
                                               Outcome *outcome = nullptr);

  Result<Image> decode(Format format, const uint8_t *data, size_t size,
                       Outcome *outcome = nullptr);

  Result<double> mse(const Image &a, const Image &b,
                     Outcome *outcome = nullptr);
  Result<double> psnr(const Image &a, const Image &b,
                      Outcome *outcome = nullptr);
  Result<SsimResult> ssim(const Image &a, const Image &b,
                          Outcome *outcome = nullptr);

  // ---- 强制指定后端（对拍与调参用）----
  // 不回退：失败就返回失败原因。自动回退会把「CUDA 与 CPU 的数字不一样」
  // 这类问题变成「CUDA 悄悄没用上」，对拍就失去意义。
  Result<std::vector<uint8_t>> encodeWith(BackendKind kind, Format format,
                                          const Image &image,
                                          const EncodeParams &params);
  Result<std::vector<uint8_t>> encodeTiffStripWith(BackendKind kind,
                                                   const Image &image,
                                                   int first_row, int rows,
                                                   const EncodeParams &params);
  Result<Image> decodeWith(BackendKind kind, Format format, const uint8_t *data,
                           size_t size);
  Result<double> mseWith(BackendKind kind, const Image &a, const Image &b);
  Result<double> psnrWith(BackendKind kind, const Image &a, const Image &b);
  Result<SsimResult> ssimWith(BackendKind kind, const Image &a, const Image &b);

  // 默认优先级：GPU 快的排前面，CPU 永远最后。
  static std::vector<BackendKind> defaultOrder();

  // 当前顺序下**这组参数**能被表达的能力集合（各后端取并集：自动模式
  // 遇到不支持的后端会换下一个，所以只要有一个能表达，请求就能成立）。
  EncodeCapabilities encodeCapabilities() const;

  // 某一个后端自己的能力表（该后端未编译进来时返回全 false）。
  //
  // 强制后端（`ico ... --backend X` / `encodeWith`）必须用这个而不是
  // `encodeCapabilities()`：并集回答的是「顺序里有没有人能表达」，而强制
  // 模式没有兜底顺序。用并集就会把「CPU 才有的进阶参数」报告给绑死在这台
  // GPU 上的 lane，让上层以为能派单，实际编码时被 `encodeWith` 拒绝。
  EncodeCapabilities encodeCapabilitiesFor(BackendKind kind) const;

  // 某个后端能否表达这组参数。支持 → 返回空；不支持 → UnsupportedFormat
  // 并写清**具体是哪一项**，让上层日志能回答「为什么这条 lane 不接这一单」。
  Result<void> checkEncodeParams(BackendKind kind, Format format,
                                 const EncodeParams &params) const;

private:
  ICodecBackend *find(BackendKind kind) const;

  // 依次尝试；supports 判断该后端是否接这一单，只有「后端不可用 / 不支持 /
  // 资源不足」才换下一个（见 router.cpp 的 retryable）。
  template <class T, class Supports, class Fn> Result<T>
  tryEach(const char *stage, Outcome *outcome, Supports &&supports, Fn &&fn);

  std::vector<std::unique_ptr<ICodecBackend>> backends_;
  std::vector<BackendKind> order_;
};

} // namespace offload
} // namespace image_codec
