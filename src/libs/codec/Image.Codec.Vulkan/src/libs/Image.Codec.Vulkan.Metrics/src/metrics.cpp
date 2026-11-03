// File      : metrics.cpp
// Brief     : MSE / PSNR / SSIM / MS-SSIM 的 Vulkan 实现（按行 band 流式上传）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 为什么按 band 而不是整幅上传：Merge.bmp 是 32000x19656x3（1.89 GB），
// 两张图同时驻留设备就是 3.8 GB；按 band 上传后显存占用只与 band_rows 有关、
// 与图像高度无关，大图与小图走同一条代码路径。
//
// 归约形态：每个 work-group 把自己的累加量写进 partials（ssim / cs / sq 三段），
// 宿主再按 group 顺序求和。Vulkan 的 GLSL 没有 float64 的原子加，
// 「共享内存树形归约 + 宿主收尾」既确定又不需要额外扩展。
#include "metrics.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

#include "metrics_common.h"
#include "vk_buffer.h"
#include "vk_codec_device.h"
#include "vk_pipeline.h"

// 由 CMake 从 kernels/metrics.comp 生成（构建产物，不提交进仓库）。
#include "metrics_spv_data.h"

namespace image_codec {
namespace vulkan {
namespace metrics {

namespace {

// 与 kernels/metrics.comp 的 local_size_x 一致。
constexpr size_t kLocalSize = 256;
// 只是防止 global size 无意义地铺满超大图；每个 invocation 本来就是网格跨步。
constexpr size_t kMaxGroups = 4096;

bool validate(const ImageBuffer &a, const ImageBuffer &b, std::string *err) {
  if (!a.valid() || !b.valid()) {
    if (err != nullptr) *err = "metrics: invalid image buffer";
    return false;
  }
  if (a.width != b.width || a.height != b.height || a.channels != b.channels) {
    if (err != nullptr) *err = "metrics: size/channel mismatch";
    return false;
  }
  if (static_cast<long long>(a.width) * a.channels > INT_MAX / 2) {
    if (err != nullptr) *err = "metrics: image row too wide for the shader";
    return false;
  }
  return true;
}

// 一次统计期间复用的设备资源（各尺度共用一套，靠 ensure() 只增长不缩小）。
struct Scratch {
  Buffer a;
  Buffer b;
  Buffer weights;
  Buffer partials;
  Pipeline pipeline;
  bool ready = false;

  void bind(Device *device) {
    a = Buffer(device, 16);
    b = Buffer(device, 16);
    weights = Buffer(device, sizeof(double) * kWindow);
    partials = Buffer(device, 16);
  }
};

// 与 kernels/metrics.comp 的 push constant 布局逐字段对应（8 × 4 字节 = 32）。
struct BandParams {
  int32_t width;
  int32_t height;
  int32_t channels;
  int32_t y0;
  int32_t band_rows;
  int32_t halo;
  int32_t with_window;
  uint32_t groups;
};

bool initPipeline(Device &device, Pipeline *pipeline, std::string *error) {
  std::vector<BufferBinding> bindings = {
      {0, false}, // a
      {1, false}, // b
      {2, false}, // 高斯权重
      {3, true},  // partials
  };
  return pipeline->init(device, vic_metrics, vic_metrics_size, bindings,
                        sizeof(BandParams), error);
}

// 单尺度统计（GPU）。with_window == 0 时只累加平方误差。
bool scaleStats(const ImageBuffer &a, const ImageBuffer &b, Device &device,
                Scratch &scratch, int band_rows, bool with_window,
                ScaleStats *out, std::string *err) {
  *out = ScaleStats{};

  const int width = a.width;
  const int height = a.height;
  const int channels = a.channels;

  const size_t row_bytes = a.stride();
  const int step = std::clamp(band_rows, 1, height);
  const int rows = step + 2 * kHalo;
  const size_t band_bytes = static_cast<size_t>(rows) * row_bytes;

  const size_t samples = static_cast<size_t>(step) *
                         static_cast<size_t>(width) *
                         static_cast<size_t>(channels);
  size_t groups = (samples + kLocalSize - 1) / kLocalSize;
  groups = std::clamp<size_t>(groups, 1, kMaxGroups);

  if (!scratch.a.ensure(band_bytes) || !scratch.b.ensure(band_bytes) ||
      !scratch.weights.ok() ||
      !scratch.partials.ensure(sizeof(double) * 3 * groups)) {
    if (err != nullptr) {
      *err = "metrics: device buffer allocation failed (" + scratch.a.error() +
             scratch.b.error() + scratch.partials.error() + ")";
    }
    return false;
  }

  double gauss[kWindow];
  gaussianWeights11(gauss);
  if (!scratch.weights.upload(gauss, sizeof(gauss))) {
    if (err != nullptr) *err = "metrics: upload of gaussian weights failed";
    return false;
  }

  // 复用同一块 host 缓冲：每个 band 只填 (step + 2*halo) 行，越界行复制边缘行。
  std::vector<uint8_t> host_a(band_bytes);
  std::vector<uint8_t> host_b(band_bytes);

  double total_ssim = 0.0;
  double total_cs = 0.0;
  double total_sq = 0.0;

  std::vector<double> partials(3 * groups, 0.0);

  for (int y0 = 0; y0 < height; y0 += step) {
    const int expect = std::min(step, height - y0);

    for (int r = 0; r < rows; ++r) {
      int src_row = y0 - kHalo + r;
      if (src_row < 0) src_row = 0;
      if (src_row >= height) src_row = height - 1;

      std::memcpy(host_a.data() + static_cast<size_t>(r) * row_bytes,
                  a.data.data() + static_cast<size_t>(src_row) * row_bytes,
                  row_bytes);
      std::memcpy(host_b.data() + static_cast<size_t>(r) * row_bytes,
                  b.data.data() + static_cast<size_t>(src_row) * row_bytes,
                  row_bytes);
    }

    if (!scratch.a.upload(host_a.data(), band_bytes) ||
        !scratch.b.upload(host_b.data(), band_bytes)) {
      if (err != nullptr) *err = "metrics: band upload failed";
      return false;
    }

    scratch.partials.zero();

    BandParams params{};
    params.width = width;
    params.height = height;
    params.channels = channels;
    params.y0 = y0;
    params.band_rows = expect;
    params.halo = kHalo;
    params.with_window = with_window ? 1 : 0;
    params.groups = static_cast<uint32_t>(groups);

    std::vector<VkBuffer> buffers = {
        scratch.a.vkBuffer(),
        scratch.b.vkBuffer(),
        scratch.weights.vkBuffer(),
        scratch.partials.vkBuffer(),
    };

    std::string local_err;
    if (!scratch.pipeline.dispatch(buffers, &params, sizeof(params),
                                   static_cast<uint32_t>(groups), &local_err)) {
      if (err != nullptr) *err = "metrics: " + local_err;
      return false;
    }

    if (!scratch.partials.download(partials.data(),
                                   sizeof(double) * 3 * groups)) {
      if (err != nullptr) *err = "metrics: partial sum download failed";
      return false;
    }

    // 按 group 顺序累加：GPU 侧每个 group 内部的顺序是固定的（树形归约），
    // 这里再把 group 串起来，结果与运行次数无关，可重复。
    for (size_t g = 0; g < groups; ++g) {
      total_ssim += partials[g];
      total_cs += partials[groups + g];
      total_sq += partials[2 * groups + g];
    }
  }

  out->ssim_sum = total_ssim;
  out->cs_sum = total_cs;
  out->sq_sum = total_sq;
  out->samples = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) *
                 static_cast<uint64_t>(channels);
  return true;
}

// 一次性准备：设备 + 管线 + 缓冲。失败时 err 说清是哪一步，
// 上层据此判断「后端不可用」而不是「图有问题」。
bool prepare(Device &device, Scratch &scratch, const Options &options,
             std::string *err) {
  std::string local_err;
  if (!device.init(options.device_index, &local_err)) {
    if (err != nullptr) *err = local_err;
    return false;
  }
  // 指标必须用 double 累加（与 CPU 参考逐位一致的前提），没有 shaderFloat64
  // 就只能明确失败：上层据此回退 CPU 指标，而不是拿一份 float 结果冒充。
  // 编解码不受这条限制，所以这个检查只在指标入口做。
  if (!device.caps().fp64) {
    if (err != nullptr) {
      *err = "metrics: device '" + device.caps().name +
             "' does not support shaderFloat64 (metrics need double "
             "accumulation); fall back to the CPU metrics";
    }
    return false;
  }
  device.setMemoryLimit(options.memory_limit);

  scratch.bind(&device);
  if (!initPipeline(device, &scratch.pipeline, &local_err)) {
    if (err != nullptr) *err = "metrics: " + local_err;
    return false;
  }
  scratch.ready = true;
  return true;
}

} // namespace

bool mse(const ImageBuffer &a, const ImageBuffer &b, const Options &options,
         double *out, std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "metrics: null output";
    return false;
  }
  *out = 0.0;

  if (!validate(a, b, err)) return false;
  if (!options.use_gpu) return reference::mse(a, b, out, err);

  Device device;
  Scratch scratch;
  if (!prepare(device, scratch, options, err)) return false;

  ScaleStats s;
  if (!scaleStats(a, b, device, scratch, options.band_rows,
                  /*with_window=*/false, &s, err)) {
    return false;
  }

  const double n = static_cast<double>(s.samples);
  if (!(n > 0.0)) {
    if (err != nullptr) *err = "metrics: empty image";
    return false;
  }
  *out = s.sq_sum / n;
  return true;
}

bool psnr(const ImageBuffer &a, const ImageBuffer &b, const Options &options,
          double *out, std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "metrics: null output";
    return false;
  }
  double m = 0.0;
  if (!mse(a, b, options, &m, err)) return false;
  *out = psnrFromMse(m);
  return true;
}

bool ssim(const ImageBuffer &a, const ImageBuffer &b, const Options &options,
          SsimResult *out, std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "metrics: null output";
    return false;
  }
  *out = SsimResult{};

  if (!validate(a, b, err)) return false;
  if (!options.use_gpu) return reference::ssim(a, b, out, err);

  Device device;
  Scratch scratch;
  if (!prepare(device, scratch, options, err)) return false;

  // ---- 第 0 级：SSIM 本身 + MS-SSIM 第 0 级的 CS（同一次 11x11 窗统计）----
  ScaleStats s;
  if (!scaleStats(a, b, device, scratch, options.band_rows,
                  /*with_window=*/true, &s, err)) {
    return false;
  }
  const double n0 = static_cast<double>(s.samples);
  if (!(n0 > 0.0)) {
    if (err != nullptr) *err = "metrics: empty image";
    return false;
  }
  out->ssim = s.ssim_sum / n0;

  // ---- MS-SSIM：逐级 2x2 均值下采样，只有最末一级取完整 SSIM ----
  const int levels = msSsimLevels(a.width, a.height);
  out->levels = levels;

  double cs[kMsSsimMaxLevels] = {0.0, 0.0, 0.0, 0.0, 0.0};
  cs[0] = s.cs_sum / n0;

  double last_ssim = out->ssim;

  // 层级间只保留当前层与下一层：整座金字塔在 32000x19656 上要白占约 630 MB。
  ImageBuffer cur_a;
  ImageBuffer cur_b;
  ImageBuffer next_a;
  ImageBuffer next_b;

  for (int level = 1; level < levels; ++level) {
    const ImageBuffer &src_a = level == 1 ? a : cur_a;
    const ImageBuffer &src_b = level == 1 ? b : cur_b;

    if (!downsample2x(src_a, &next_a) || !downsample2x(src_b, &next_b)) {
      if (err != nullptr) *err = "metrics: pyramid downsampling failed";
      return false;
    }

    ScaleStats sl;
    if (!scaleStats(next_a, next_b, device, scratch, options.band_rows,
                    /*with_window=*/true, &sl, err)) {
      return false;
    }

    const double nl = static_cast<double>(sl.samples);
    cs[level] = sl.cs_sum / nl;
    last_ssim = sl.ssim_sum / nl;

    cur_a = std::move(next_a);
    cur_b = std::move(next_b);
  }

  out->ms_ssim = msSsimFromScales(cs, last_ssim, levels);
  return true;
}

} // namespace metrics
} // namespace vulkan
} // namespace image_codec
