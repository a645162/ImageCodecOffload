/*
 * File: png_filter_ocl.h
 * Brief: Png filter ocl.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// png_filter_ocl.h - PNG 行滤波的 OpenCL 实现（自适应选择 + 生成 filtered 流）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"
#include "ocl_device.h"

namespace image_codec {
namespace ocl {
namespace png {

// 内嵌 kernel 源码（由 CMake 生成的 cl_strings.cpp 提供）。
extern const char kPngFilterSource[];

// 一次构建一个 filtered 流。设备/程序惰性初始化，失败原因保留在 lastError()，
// 由上层决定「回退 CPU 参考实现」还是直接报错（绝不静默降级）。
class PngFilterOcl {
public:
  explicit PngFilterOcl(int device_index = 0);
  ~PngFilterOcl();

  PngFilterOcl(const PngFilterOcl &) = delete;
  PngFilterOcl &operator=(const PngFilterOcl &) = delete;

  // 生成 filtered 流：height 行，每行 (1 + width*channels) 字节。
  // filter_type: -1 = 逐行自适应（最小绝对值和），0..4 = 固定滤波器。
  // filters_out 可选：回报每行实际使用的滤波器。
  bool buildFilteredStream(const ImageBuffer &img, int filter_type,
                           std::vector<uint8_t> *out,
                           std::vector<uint8_t> *filters_out);

  // 自适应时每行的 work-group 大小（调参旋钮）。0 = 256。
  void setBlockThreads(int threads) { block_threads_ = threads; }

  const std::string &lastError() const { return err_; }

private:
  bool initOnce();

  int device_index_ = 0;
  int block_threads_ = 256;
  bool init_done_ = false;
  std::string err_;
  OclDevice dev_;
  cl_program prog_ = nullptr;
  cl_kernel k_rows_ = nullptr;
  cl_kernel k_sums_ = nullptr;
};

} // namespace png
} // namespace ocl
} // namespace image_codec
