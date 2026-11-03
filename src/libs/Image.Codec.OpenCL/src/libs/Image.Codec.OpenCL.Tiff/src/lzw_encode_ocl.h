/*
 * File: lzw_encode_ocl.h
 * Brief: Lzw encode ocl.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/lzw_encode_ocl.h - GPU LZW 编码（strip/tile 并行 kernel，内核源码内嵌）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <CL/cl.h>

#include "ocl_device.h"

namespace image_codec {
namespace tiff {

// GPU LZW 编码器。设备与 kernel 只初始化一次，缓冲区按需创建/释放。
// 注意：Intel NEO 驱动对单 work-item 的缓冲分配有大小限制，分块(block_cap)
// 与 RowsPerStrip/tile 尺寸可调以规避 CL_OUT_OF_RESOURCES。
class LzwEncodeOcl {
public:
  explicit LzwEncodeOcl(int device_index = 0);
  ~LzwEncodeOcl();

  LzwEncodeOcl(const LzwEncodeOcl &) = delete;
  LzwEncodeOcl &operator=(const LzwEncodeOcl &) = delete;

  // 编码整幅 top-down 图像（width*channels 字节/行）为若干等长 strip。
  // channels 必须是调用方真实写出的样本数（1..4）：旧实现写死 3，于是灰度图
  // 每行只搬前 1/3 的字节、RGBA 图少搬 1/4，产物能「编出来」但内容是错的。
  // rows_per_strip 必须整除 height。
  // out_sizes[i] / out_data 为每 strip 的压缩字节（按 strip 顺序拼接）。
  // 返回 0 成功，负值失败。
  int encodeStrips(const uint8_t *rgb, int width, int height, int channels,
                   int rows_per_strip, std::vector<uint32_t> &out_sizes,
                   std::vector<uint8_t> &out_data);

  // 编码为 tile 布局（方形 tile，tile_w == tile_h）。图像自动填充到 tile 整数倍。
  // out_sizes[i] / out_data 为每 tile 压缩字节（行主序，tile 内行主序）。
  // 返回 0 成功，负值失败。
  int encodeTiles(const uint8_t *rgb, int width, int height, int channels,
                  int tile_size, std::vector<uint32_t> &out_sizes,
                  std::vector<uint8_t> &out_data);

  const char *lastError() const { return err_.c_str(); }

private:
  bool initOnce();
  void releaseBuffers();
  void setError(const std::string &msg) { err_ = msg; }

  int device_index_;
  std::string err_;
  bool init_done_ = false;

  // OpenCL 对象（持久）。OclDevice 按值持有（构造/析构在 .cpp 中完成）。
  OclDevice dev_;
  cl_context ctx_ = nullptr;
  cl_command_queue queue_ = nullptr;
  cl_device_id device_id_ = nullptr;
  cl_program strip_program_ = nullptr;
  cl_kernel strip_kernel_ = nullptr;
  cl_program tile_program_ = nullptr;
  cl_kernel tile_kernel_ = nullptr;
  int built_tile_h_ = -1;

  // 当前操作缓冲区（按需创建）。
  cl_mem in_mem_ = nullptr;
  cl_mem dict_mem_ = nullptr;
  cl_mem out_mem_ = nullptr;
  cl_mem sizes_mem_ = nullptr;
  size_t block_cap_ = 0;
  size_t unit_bytes_ = 0; // strip_bytes 或 tile_bytes
  size_t max_unit_ = 0;   // max_strip 或 max_tile

  // 计算 safe block_cap（受 CL_DEVICE_MAX_MEM_ALLOC_SIZE 限制）。
  size_t computeBlockCap(size_t unit_bytes, size_t max_unit) const;
};

} // namespace tiff
} // namespace image_codec
