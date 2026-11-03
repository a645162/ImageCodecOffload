// File      : png_filter_vk.cpp
// Brief     : PNG 行滤波的 Vulkan 实现
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#include "png_filter_vk.h"

#include <algorithm>
#include <cstring>

// 由 CMake 从 kernels/png_filter.comp 生成（构建产物，不提交进仓库）。
#include "png_filter_spv_data.h"

namespace image_codec {
namespace vulkan {
namespace png {

namespace {

// 与 kernels/png_filter.comp 的 local_size_x 一致。
constexpr size_t kLocalSize = 256;

struct FilterParams {
  int32_t row_bytes = 0;
  int32_t channels = 0;
  int32_t band_rows = 0;
  int32_t fixed_filter = -1;
  int32_t mode = 0;
  int32_t pad0 = 0;
  int32_t pad1 = 0;
  int32_t pad2 = 0;
};

size_t alignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// 与宿主参考实现 chooseAdaptiveFilters() 完全同一条规则：
// 严格小于，平局取下标更小的滤波类型。两边不同就会出现「GPU 选 3、CPU 选 4」
// 这种看起来都对、但 filtered 流一个字节都对不上的结果。
int pickFilter(const uint32_t *scores) {
  uint32_t best = 0xFFFFFFFFu;
  int best_filter = 0;
  for (int f = 0; f < 5; ++f) {
    if (scores[f] < best) {
      best = scores[f];
      best_filter = f;
    }
  }
  return best_filter;
}

} // namespace

bool filterRowsOnGpu(const ImageBuffer &img, const FilterOptions &options,
                     std::vector<uint8_t> *out, std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "PNG: null output";
    return false;
  }
  out->clear();
  if (!img.valid()) {
    if (err != nullptr) *err = "PNG: invalid image buffer";
    return false;
  }
  if (options.filter < -1 || options.filter > 4) {
    if (err != nullptr) *err = "PNG: filter must be -1 (adaptive) or 0..4";
    return false;
  }

  Device device;
  std::string local_err;
  if (!device.init(options.device_index, &local_err)) {
    if (err != nullptr) *err = "PNG: " + local_err;
    return false;
  }
  device.setMemoryLimit(options.memory_limit);

  const int width = img.width;
  const int height = img.height;
  const int channels = img.channels;
  const size_t row_bytes = img.stride();
  const int step = std::clamp(options.band_rows, 1, height);
  const int band_rows = step + 1; // 多一行：band 第 0 行是「上一行」

  Buffer src(&device, static_cast<size_t>(band_rows) * row_bytes);
  Buffer dst(&device, static_cast<size_t>(step) * row_bytes);
  Buffer scores(&device, static_cast<size_t>(step) * 5 * sizeof(uint32_t));
  if (!src.ok() || !dst.ok() || !scores.ok()) {
    if (err != nullptr) {
      *err = "PNG: device buffer allocation failed (" + src.error() +
             dst.error() + scores.error() + ")";
    }
    return false;
  }

  Pipeline pipeline;
  const std::vector<BufferBinding> bindings = {
      {0, false}, // src
      {1, true},  // dst
      {2, true},  // scores
  };
  if (!pipeline.init(device, vic_png_filter, vic_png_filter_size, bindings,
                     sizeof(FilterParams), &local_err)) {
    if (err != nullptr) *err = "PNG: " + local_err;
    return false;
  }

  const size_t row_stride_out = row_bytes + 1;
  out->assign(static_cast<size_t>(height) * row_stride_out, 0);

  const size_t band_bytes = static_cast<size_t>(band_rows) * row_bytes;
  std::vector<uint8_t> host_band(band_bytes, 0);
  std::vector<uint32_t> host_scores(static_cast<size_t>(step) * 5, 0);
  std::vector<uint32_t> host_words;
  const size_t dst_words =
      alignUp(static_cast<size_t>(step) * row_bytes, 4) / 4;
  host_words.resize(dst_words, 0);

  for (int y0 = 0; y0 < height; y0 += step) {
    const int expect = std::min(step, height - y0);

    // band 第 0 行 = 上一行（整图 y0-1）。第一行的「上一行」按 PNG 规范是
    // 全零，不是重复第一行——这两者只在第一行有差别，出错了 PNG 依然能
    // 打开，但 filtered 流与参考实现逐字节对不上。
    if (y0 == 0) {
      std::memset(host_band.data(), 0, row_bytes);
    } else {
      std::memcpy(host_band.data(),
                  img.data.data() + static_cast<size_t>(y0 - 1) * row_bytes,
                  row_bytes);
    }
    std::memcpy(host_band.data() + row_bytes,
                img.data.data() + static_cast<size_t>(y0) * row_bytes,
                static_cast<size_t>(expect) * row_bytes);

    if (!src.upload(host_band.data(), band_bytes)) {
      if (err != nullptr) *err = "PNG: band upload failed";
      return false;
    }
    scores.zero();
    dst.zero();

    FilterParams params{};
    params.row_bytes = static_cast<int32_t>(row_bytes);
    params.channels = channels;
    params.band_rows = expect;
    params.fixed_filter = options.filter;

    const std::vector<VkBuffer> buffers = {src.vkBuffer(), dst.vkBuffer(),
                                           scores.vkBuffer()};

    if (options.filter < 0) {
      // ---- 打分：每个 (行, 滤波类型) 一个 work-group ----
      params.mode = 0;
      const uint32_t groups = static_cast<uint32_t>(expect) * 5u;
      if (!pipeline.dispatch(buffers, &params, sizeof(params), groups,
                             &local_err)) {
        if (err != nullptr) *err = "PNG: score dispatch failed: " + local_err;
        return false;
      }
      if (!scores.download(host_scores.data(),
                           host_scores.size() * sizeof(uint32_t))) {
        if (err != nullptr) *err = "PNG: score download failed";
        return false;
      }
    }

    // ---- 应用：一个 invocation 一个 uint 字 ----
    params.mode = 1;
    const size_t words = dst_words;
    const uint32_t groups =
        static_cast<uint32_t>((words + kLocalSize - 1) / kLocalSize);
    if (!pipeline.dispatch(buffers, &params, sizeof(params), groups,
                           &local_err)) {
      if (err != nullptr) *err = "PNG: apply dispatch failed: " + local_err;
      return false;
    }
    if (!dst.download(host_words.data(),
                      std::min(dst_words, words) * sizeof(uint32_t))) {
      if (err != nullptr) *err = "PNG: filtered data download failed";
      return false;
    }

    // ---- 组装：每行写 1 字节滤波类型，再拷 row_bytes 字节 ----
    const uint8_t *filtered =
        reinterpret_cast<const uint8_t *>(host_words.data());
    for (int r = 0; r < expect; ++r) {
      const int filter =
          options.filter >= 0
              ? options.filter
              : pickFilter(&host_scores[static_cast<size_t>(r) * 5]);
      uint8_t *dst_row =
          out->data() + static_cast<size_t>(y0 + r) * row_stride_out;
      dst_row[0] = static_cast<uint8_t>(filter);
      std::memcpy(dst_row + 1, filtered + static_cast<size_t>(r) * row_bytes,
                  row_bytes);
    }
  }
  return true;
}

} // namespace png
} // namespace vulkan
} // namespace image_codec
