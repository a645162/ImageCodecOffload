// File      : png_codec.cpp
// Brief     : PNG 编解码门面实现
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#include "png_codec.h"

#include "png_container.h"
#include "png_filter_vk.h"

namespace image_codec {
namespace vulkan {
namespace png {

bool encodePng(const ImageBuffer &img, const PngEncodeOptions &options,
               std::vector<uint8_t> *out, bool *gpu_used, std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "PNG: null output";
    return false;
  }
  out->clear();
  if (gpu_used != nullptr) *gpu_used = false;
  if (!img.valid()) {
    if (err != nullptr) *err = "PNG: invalid image buffer";
    return false;
  }

  std::vector<uint8_t> filtered;
  if (options.use_gpu) {
    FilterOptions filter_options;
    filter_options.band_rows = options.band_rows;
    filter_options.filter = options.filter;
    filter_options.memory_limit = options.memory_limit;
    filter_options.device_index = options.device_index;

    std::string gpu_err;
    if (filterRowsOnGpu(img, filter_options, &filtered, &gpu_err)) {
      if (gpu_used != nullptr) *gpu_used = true;
    } else {
      // 明确记录原因；是否继续走 CPU 由调用方决定，但绝不静默——否则
      // 「Vulkan 后端编的 PNG」可能一个字都没经过 GPU，而没人看得出来。
      if (err != nullptr) *err = "PNG: Vulkan filtering failed: " + gpu_err;
      filtered.clear();
      if (!options.allow_cpu_fallback) return false;
    }
  }

  if (filtered.empty()) {
    // CPU 参考滤波：与 GPU 走的自适应规则逐位相同（同一套 5 种滤波 + Σ|signed|）。
    std::vector<uint8_t> filters;
    if (options.filter >= 0) {
      filters.assign(static_cast<size_t>(img.height),
                     static_cast<uint8_t>(options.filter));
    } else {
      filters = chooseAdaptiveFilters(img.data.data(), img.width, img.height,
                                      img.channels);
    }
    filtered = buildFilteredStream(img.data.data(), img.width, img.height,
                                   img.channels, filters);
  }
  if (filtered.empty()) {
    if (err != nullptr && err->empty()) *err = "PNG: filtering failed";
    return false;
  }

  std::string write_err;
  if (!writeFilteredPng(filtered.data(), filtered.size(), img.width, img.height,
                        img.channels, options.compression_level, *out,
                        &write_err)) {
    if (err != nullptr) *err = "PNG: " + write_err;
    return false;
  }
  return true;
}

bool decodePng(const uint8_t *data, size_t size, ImageBuffer *out,
               std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "PNG: null output";
    return false;
  }
  return readPng(data, size, *out, err);
}

} // namespace png
} // namespace vulkan
} // namespace image_codec
