// File      : tiff_predict_vk.cpp
// Brief     : TIFF Predictor=2（水平差分）的 Vulkan 实现
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#include "tiff_predict_vk.h"

#include <algorithm>
#include <climits>
#include <cstring>

// 由 CMake 从 kernels/tiff_predict.comp 生成（构建产物，不提交进仓库）。
#include "tiff_predict_spv_data.h"

namespace image_codec {
namespace vulkan {
namespace tiff {

namespace {

// 与 kernels/tiff_predict.comp 的 local_size_x 一致。
constexpr size_t kLocalSize = 256;

// 单个 band 的宿主/显存缓冲上限：着色器里 total_bytes 是 int32，
// 留足余量后 64 MB 已经远超任何真实行宽能给出的合理批大小。
constexpr size_t kMaxBandBytes = 64u * 1024u * 1024u;

// 与 kernels/tiff_predict.comp 的 push constant 布局逐字段对应（4 × 4 字节）。
struct PredictParams {
  int32_t row_bytes = 0;
  int32_t channels = 0;
  int32_t total_bytes = 0;
  int32_t pad0 = 0;
};

} // namespace

void predictRowsCpu(const uint8_t *src, size_t row_bytes, int channels,
                    int rows, uint8_t *out) {
  if (src == nullptr || out == nullptr || row_bytes == 0 || channels <= 0) {
    return;
  }
  // 逐行处理而不是整块平铺：行首的 channels 个样本没有左邻，必须按列判断。
  // 行内是串行依赖（后一个字节参考前一个字节的**原值**，这里因为读写
  // 不同缓冲而仍然是独立的），所以没有可向量化的空间——这条路径只作为
  // GPU 的对拍基线与显存不足时的回退。
  for (int r = 0; r < rows; ++r) {
    const uint8_t *in_row = src + static_cast<size_t>(r) * row_bytes;
    uint8_t *out_row = out + static_cast<size_t>(r) * row_bytes;
    for (size_t i = 0; i < row_bytes; ++i) {
      const int left = i >= static_cast<size_t>(channels)
                           ? static_cast<int>(in_row[i - channels])
                           : 0;
      out_row[i] =
          static_cast<uint8_t>((static_cast<int>(in_row[i]) - left) & 0xFF);
    }
  }
}

void predictRowsCpuInPlace(uint8_t *data, size_t row_bytes, int channels,
                           int rows) {
  if (data == nullptr || row_bytes == 0 || channels <= 0) return;
  if (static_cast<size_t>(channels) > 4u) return; // 本项目样本数只有 1..4
  for (int r = 0; r < rows; ++r) {
    uint8_t *row = data + static_cast<size_t>(r) * row_bytes;
    // 每个通道记住**原始**左邻字节：原地写会覆盖 row[i-channels]，
    // 直接读它等于拿差分结果去减，越算越错。
    uint8_t left[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < row_bytes; ++i) {
      const size_t c = i % static_cast<size_t>(channels);
      const uint8_t current = row[i];
      row[i] =
          static_cast<uint8_t>((static_cast<int>(current) - left[c]) & 0xFF);
      left[c] = current;
    }
  }
}

void unpredictRowsCpu(uint8_t *data, size_t row_bytes, int channels, int rows) {
  if (data == nullptr || row_bytes == 0 || channels <= 0) return;
  for (int r = 0; r < rows; ++r) {
    uint8_t *row = data + static_cast<size_t>(r) * row_bytes;
    // 还原必须**原地顺序**：s[i] 依赖刚算出来的 s[i-channels]。
    for (size_t i = static_cast<size_t>(channels); i < row_bytes; ++i) {
      row[i] = static_cast<uint8_t>(row[i] + row[i - channels]);
    }
  }
}

bool PredictorGpu::init(size_t memory_limit, int device_index,
                        std::string *error) {
  error_.clear();
  ready_ = false;

  std::string local;
  // device_index < 0：按类型自动选（独显优先）。写死 0 在「核显排在前面」
  // 的机器上会选到一个没有 fp64 的设备，然后整条编解码链路莫名其妙不可用。
  if (!device_.init(device_index, &local)) {
    error_ = "TIFF: " + local;
    if (error != nullptr) *error = error_;
    return false;
  }
  device_.setMemoryLimit(memory_limit);

  // Buffer 的默认构造没有设备（ensure() 无法凭空绑定），必须先显式绑定。
  src_ = Buffer(&device_, 16);
  dst_ = Buffer(&device_, 16);

  const std::vector<BufferBinding> bindings = {
      {0, false}, // src
      {1, true},  // dst
  };
  if (!pipeline_.init(device_, vic_tiff_predict, vic_tiff_predict_size,
                      bindings, sizeof(PredictParams), &local)) {
    error_ = "TIFF: " + local;
    if (error != nullptr) *error = error_;
    return false;
  }

  ready_ = true;
  return true;
}

bool PredictorGpu::run(uint8_t *data, size_t row_bytes, int channels, int rows,
                       int band_rows, std::string *error) {
  error_.clear();
  if (!ready_) {
    error_ = "TIFF: predictor pipeline is not initialized";
    if (error != nullptr) *error = error_;
    return false;
  }
  if (data == nullptr || row_bytes == 0 || channels <= 0 || rows <= 0) {
    error_ = "TIFF: invalid predictor input";
    if (error != nullptr) *error = error_;
    return false;
  }
  if (row_bytes > static_cast<size_t>(INT_MAX) / 2) {
    error_ = "TIFF: row is too wide for the predictor shader";
    if (error != nullptr) *error = error_;
    return false;
  }

  // 批大小同时受「调用方要求」与「int32 push constant 上限」约束。
  size_t row_step = band_rows > 0 ? static_cast<size_t>(band_rows) : 256u;
  const size_t max_rows = std::max<size_t>(1u, kMaxBandBytes / row_bytes);
  row_step = std::min(std::min(row_step, max_rows), static_cast<size_t>(rows));

  for (size_t y = 0; y < static_cast<size_t>(rows); y += row_step) {
    const int count =
        static_cast<int>(std::min(row_step, static_cast<size_t>(rows) - y));
    const size_t band_bytes = static_cast<size_t>(count) * row_bytes;
    const size_t words = (band_bytes + 3u) / 4u;

    if (!src_.ensure(words * 4u) || !dst_.ensure(words * 4u)) {
      error_ = "TIFF: device buffer allocation failed (" + src_.error() +
               dst_.error() + ")";
      if (error != nullptr) *error = error_;
      return false;
    }
    if (!src_.upload(data + y * row_bytes, band_bytes)) {
      error_ = "TIFF: predictor upload failed";
      if (error != nullptr) *error = error_;
      return false;
    }
    dst_.zero();

    // 必须在 ensure() 之后再取句柄：缓冲增长会重建 VkBuffer，提前取到的
    // 句柄已经被销毁，派发时会直接让设备丢失。
    const std::vector<VkBuffer> buffers = {src_.vkBuffer(), dst_.vkBuffer()};

    PredictParams params{};
    params.row_bytes = static_cast<int32_t>(row_bytes);
    params.channels = channels;
    params.total_bytes = static_cast<int32_t>(band_bytes);

    const uint32_t groups = static_cast<uint32_t>(
        words / kLocalSize + (words % kLocalSize ? 1u : 0u));
    std::string local;
    if (!pipeline_.dispatch(buffers, &params, sizeof(params),
                            std::max<uint32_t>(groups, 1u), &local)) {
      error_ = "TIFF: predictor dispatch failed: " + local;
      if (error != nullptr) *error = error_;
      return false;
    }

    words_.resize(words);
    if (!dst_.download(words_.data(), words * 4u)) {
      error_ = "TIFF: predictor download failed";
      if (error != nullptr) *error = error_;
      return false;
    }
    std::memcpy(data + y * row_bytes, words_.data(), band_bytes);
  }

  return true;
}

} // namespace tiff
} // namespace vulkan
} // namespace image_codec
