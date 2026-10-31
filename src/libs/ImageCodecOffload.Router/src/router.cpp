// router.cpp - 后端注册、选择与回退。
#include "router.h"

#include <optional>

#include "cpu_backend.h"
#include "cuda_backend.h"
#include "opencl_backend.h"
#include "vulkan_backend.h"

namespace image_codec {
namespace offload {

namespace {

// 「这个后端明显不适合这一单」的三类失败（写进 notes 后立刻换下一个）：
//   * 后端不可用：没设备 / 没编译进来 / 缺驱动；
//   * 不支持该格式（或该方向，例如没有 nvJPEG 时的 JPEG 编码）；
//   * 资源不足：显存装不下这张图，CPU 还能用内存顶上。
//
// 其余（DecodeFailed / EncodeFailed / Io / Internal）属于「这个后端做砸了」：
// 它们**不阻止**继续试下一个后端（否则一张 1.9 GB 的图只要 OpenCL 分配不出
// 设备缓冲，就连 CPU 都不试了），但会被记进 notes 并作为「全都失败」时的
// 报错返回——一个后端算错却被另一个后端悄悄救回来，才等于把 bug 藏起来。
bool retryable(Status status) {
  return status == Status::BackendUnavailable ||
         status == Status::UnsupportedFormat || status == Status::OutOfMemory;
}

std::string describeFailure(BackendKind kind, const Error &error) {
  std::string note = backendName(kind);
  note += ": ";
  note += statusName(error.code);
  if (!error.message.empty()) {
    note += " (";
    note += error.message;
    note += ")";
  }
  return note;
}

} // namespace

template <class T, class Supports, class Fn>
Result<T> Router::tryEach(const char *stage, Outcome *outcome,
                          Supports &&supports, Fn &&fn) {
  std::vector<std::string> notes;
  bool tried_any = false;
  // 第一个「这个后端做砸了」的错误：不拦路，但如果所有后端都失败，
  // 它比「没有后端支持」更有信息量，作为最终报错返回。
  std::optional<Error> hard_error;

  for (BackendKind kind : order_) {
    ICodecBackend *backend = find(kind);
    if (backend == nullptr) {
      notes.push_back(std::string(backendName(kind)) +
                      ": not built into this build");
      continue;
    }

    if (!supports(*backend)) {
      notes.push_back(std::string(backendName(kind)) +
                      ": format/direction not supported");
      continue;
    }

    tried_any = true;
    Result<T> result = fn(*backend);
    if (result) {
      if (outcome != nullptr) {
        outcome->used = kind;
        outcome->notes = std::move(notes);
      }
      return result;
    }

    const Error error = result.error();
    notes.push_back(describeFailure(kind, error));

    // 参数非法是调用方的错，换后端也一样会错，立刻返回（不掩盖调用方的 bug）。
    if (error.code == Status::InvalidArgument) {
      if (outcome != nullptr) {
        outcome->used = kind;
        outcome->notes = std::move(notes);
      }
      return std::unexpected(error);
    }

    // 其余「做砸了」的失败只记账，仍然给后面的后端一次机会。
    if (!retryable(error.code) && !hard_error.has_value()) {
      hard_error = error;
    }
  }

  if (outcome != nullptr) {
    outcome->used = BackendKind::Cpu;
    outcome->notes = std::move(notes);
  }

  if (!tried_any) {
    return fail<T>(Status::UnsupportedFormat,
                   std::string("router: no backend supports this request (") +
                       stage + ")");
  }
  if (hard_error.has_value()) {
    // 保留第一个「做砸了」的后端的具体原因（原始状态码不变，上层仍能分支）。
    return std::unexpected(*hard_error);
  }
  return fail<T>(Status::BackendUnavailable,
                 std::string("router: all backends failed (") + stage + ")");
}

Router::Router() {
  // 注册所有被编译进来的后端。makeXxxBackend() 在未编译时返回 nullptr，
  // 因此这里不需要 ifdef：开关只影响那个后端自己的适配文件。
  if (auto opencl = makeOpenClBackend(); opencl != nullptr) {
    backends_.push_back(std::move(opencl));
  }
  if (auto cuda = makeCudaBackend(); cuda != nullptr) {
    backends_.push_back(std::move(cuda));
  }
  if (auto vulkan = makeVulkanBackend(); vulkan != nullptr) {
    backends_.push_back(std::move(vulkan));
  }
  backends_.push_back(std::make_unique<cpu::CpuBackend>());

  order_ = defaultOrder();
}

Router::~Router() = default;

std::vector<BackendKind> Router::defaultOrder() {
  // CUDA 排第一：同一张图上 nvJPEG / GPU LZW / GPU 行滤波通常比手写 OpenCL
  // 实现快；OpenCL 第二（覆盖面最广，Intel/NVIDIA/AMD 都有）；Vulkan 第三
  // （目前只有 TIFF/PNG 与指标，JPEG 由能力表挡掉）；CPU 永远最后。
  return {BackendKind::Cuda, BackendKind::OpenCl, BackendKind::Vulkan,
          BackendKind::Cpu};
}

void Router::setOrder(std::vector<BackendKind> order) {
  std::vector<BackendKind> normalized;
  normalized.reserve(order.size() + 1);

  const auto push = [&normalized](BackendKind kind) {
    for (BackendKind existing : normalized) {
      if (existing == kind) return; // 去重：重复项没有语义
    }
    normalized.push_back(kind);
  };

  for (BackendKind kind : order) {
    if (kind == BackendKind::Cpu) continue; // CPU 由下面统一追加到末尾
    push(kind);
  }
  push(BackendKind::Cpu);

  order_ = std::move(normalized);
}

std::vector<BackendKind> Router::order() const { return order_; }

std::vector<BackendKind> Router::availableBackends() const {
  std::vector<BackendKind> out;
  out.reserve(backends_.size());
  for (const auto &backend : backends_) {
    out.push_back(backend->kind());
  }
  return out;
}

std::vector<std::pair<BackendKind, std::string>> Router::probeBackends() const {
  std::vector<std::pair<BackendKind, std::string>> out;
  out.reserve(backends_.size());
  for (const auto &backend : backends_) {
    const Result<void> ready = backend->available();
    out.emplace_back(backend->kind(),
                     ready ? std::string("ready") : ready.error().message);
  }
  return out;
}

ICodecBackend *Router::find(BackendKind kind) const {
  for (const auto &backend : backends_) {
    if (backend->kind() == kind) return backend.get();
  }
  return nullptr;
}

// ---- 编解码 ----

Result<std::vector<uint8_t>> Router::encode(Format format, const Image &image,
                                            const EncodeParams &params,
                                            Outcome *outcome) {
  // JPEG 没有 Alpha 样本：2/4 通道进 JPEG 必须由调用方先决定怎么处理
  // （丢掉还是先合成到背景上）。在这里静默降维是最坏的选择——文件能打开、
  // 画质指标也正常，只有透明度凭空消失了。
  if (format == Format::Jpeg && (image.channels == 2 || image.channels == 4)) {
    return fail<std::vector<uint8_t>>(
        Status::InvalidArgument,
        "encode: jpeg has no alpha channel; convert to 1 or 3 channels first");
  }
  return tryEach<std::vector<uint8_t>>(
      "encode", outcome,
      [format](const ICodecBackend &backend) {
        return backend.supports(format);
      },
      [&](ICodecBackend &backend) {
        return backend.encode(format, image, params);
      });
}

Result<Image> Router::decode(Format format, const uint8_t *data, size_t size,
                             Outcome *outcome) {
  return tryEach<Image>(
      "decode", outcome,
      [format](const ICodecBackend &backend) {
        return backend.supports(format);
      },
      [&](ICodecBackend &backend) {
        return backend.decode(format, data, size);
      });
}

// ---- 指标 ----

Result<double> Router::mse(const Image &a, const Image &b, Outcome *outcome) {
  return tryEach<double>(
      "mse", outcome, [](const ICodecBackend &) { return true; },
      [&](ICodecBackend &backend) { return backend.mse(a, b); });
}

Result<double> Router::psnr(const Image &a, const Image &b, Outcome *outcome) {
  return tryEach<double>(
      "psnr", outcome, [](const ICodecBackend &) { return true; },
      [&](ICodecBackend &backend) { return backend.psnr(a, b); });
}

Result<SsimResult> Router::ssim(const Image &a, const Image &b,
                                Outcome *outcome) {
  return tryEach<SsimResult>(
      "ssim", outcome, [](const ICodecBackend &) { return true; },
      [&](ICodecBackend &backend) { return backend.ssim(a, b); });
}

// ---- 强制指定后端 ----

Result<std::vector<uint8_t>> Router::encodeWith(BackendKind kind, Format format,
                                                const Image &image,
                                                const EncodeParams &params) {
  ICodecBackend *backend = find(kind);
  if (backend == nullptr) {
    return fail<std::vector<uint8_t>>(
        Status::BackendUnavailable,
        std::string("router: backend not built: ") + backendName(kind));
  }
  if (!backend->supports(format)) {
    return fail<std::vector<uint8_t>>(
        Status::UnsupportedFormat, std::string("router: ") + backendName(kind) +
                                       " does not support " +
                                       formatName(format));
  }
  if (format == Format::Jpeg && (image.channels == 2 || image.channels == 4)) {
    return fail<std::vector<uint8_t>>(
        Status::InvalidArgument,
        "encodeWith: jpeg has no alpha channel; convert to 1 or 3 channels "
        "first");
  }
  return backend->encode(format, image, params);
}

Result<Image> Router::decodeWith(BackendKind kind, Format format,
                                 const uint8_t *data, size_t size) {
  ICodecBackend *backend = find(kind);
  if (backend == nullptr) {
    return fail<Image>(Status::BackendUnavailable,
                       std::string("router: backend not built: ") +
                           backendName(kind));
  }
  if (!backend->supports(format)) {
    return fail<Image>(Status::UnsupportedFormat,
                       std::string("router: ") + backendName(kind) +
                           " does not support " + formatName(format));
  }
  return backend->decode(format, data, size);
}

Result<double> Router::mseWith(BackendKind kind, const Image &a,
                               const Image &b) {
  ICodecBackend *backend = find(kind);
  if (backend == nullptr) {
    return fail<double>(Status::BackendUnavailable,
                        std::string("router: backend not built: ") +
                            backendName(kind));
  }
  return backend->mse(a, b);
}

Result<double> Router::psnrWith(BackendKind kind, const Image &a,
                                const Image &b) {
  ICodecBackend *backend = find(kind);
  if (backend == nullptr) {
    return fail<double>(Status::BackendUnavailable,
                        std::string("router: backend not built: ") +
                            backendName(kind));
  }
  return backend->psnr(a, b);
}

Result<SsimResult> Router::ssimWith(BackendKind kind, const Image &a,
                                    const Image &b) {
  ICodecBackend *backend = find(kind);
  if (backend == nullptr) {
    return fail<SsimResult>(Status::BackendUnavailable,
                            std::string("router: backend not built: ") +
                                backendName(kind));
  }
  return backend->ssim(a, b);
}

} // namespace offload
} // namespace image_codec
