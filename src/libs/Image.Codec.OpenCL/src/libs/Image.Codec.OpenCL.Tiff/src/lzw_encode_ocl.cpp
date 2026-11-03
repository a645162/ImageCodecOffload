/*
 * File: lzw_encode_ocl.cpp
 * Brief: Lzw encode ocl.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/lzw_encode_ocl.cpp - GPU LZW 编码实现。
#include "lzw_encode_ocl.h"

#include "ocl_device.h"
#include "ocl_program.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

// 内嵌 kernel 源码（由 CMake file(READ)+configure_file 生成）。
namespace image_codec {
namespace tiff {
extern const char *lzw_encode_cl_source;
extern const char *lzw_encode_tile_cl_source;
} // namespace tiff
} // namespace image_codec

namespace image_codec {
namespace tiff {

namespace {

// 组装错误信息并打印一行诊断。
std::string makeErr(const char *what, cl_int err, const std::string &log) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%s: %s", what, OclProgram::errorName(err));
  std::string s = buf;
  if (!log.empty()) {
    s += "\n[build log]\n" + log;
  }
  return s;
}

} // namespace

LzwEncodeOcl::LzwEncodeOcl(int device_index) : device_index_(device_index) {}

LzwEncodeOcl::~LzwEncodeOcl() {
  releaseBuffers();
  if (tile_kernel_) OclProgram::release(tile_kernel_);
  if (tile_program_) OclProgram::release(tile_program_);
  if (strip_kernel_) OclProgram::release(strip_kernel_);
  if (strip_program_) OclProgram::release(strip_program_);
  if (dev_.valid()) dev_.release();
}

bool LzwEncodeOcl::initOnce() {
  if (init_done_) return true;

  cl_int err = dev_.init(device_index_);
  if (err != CL_SUCCESS) {
    setError(makeErr("OpenCL 设备初始化失败", err, ""));
    return false;
  }
  ctx_ = dev_.context();
  queue_ = dev_.queue();
  device_id_ = dev_.device();

  std::string log;
  err = OclProgram::build(ctx_, device_id_, lzw_encode_cl_source,
                          "-cl-std=CL1.2", &strip_program_, &log);
  if (err != CL_SUCCESS) {
    setError(makeErr("strip kernel 编译失败", err, log));
    return false;
  }
  err = OclProgram::createKernel(strip_program_, "lzw_encode", &strip_kernel_);
  if (err != CL_SUCCESS) {
    setError(makeErr("创建 strip kernel 失败", err, ""));
    return false;
  }
  init_done_ = true;
  return true;
}

size_t LzwEncodeOcl::computeBlockCap(size_t unit_bytes, size_t max_unit) const {
  cl_ulong max_alloc = 256ull << 20;
  clGetDeviceInfo(device_id_, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(max_alloc),
                  &max_alloc, nullptr);
  const size_t per_unit = unit_bytes + max_unit;
  size_t cap = 512; // 默认分块上限
  if (per_unit > 0)
    cap = std::min(cap, static_cast<size_t>(max_alloc / per_unit));
  // dict 缓冲区：cap * 4096 * 4 字节
  cap = std::min(cap, static_cast<size_t>(max_alloc / (4096ull * 4)));
  cap = std::max<size_t>(cap, 1);
  return cap;
}

void LzwEncodeOcl::releaseBuffers() {
  if (sizes_mem_) clReleaseMemObject(sizes_mem_);
  if (out_mem_) clReleaseMemObject(out_mem_);
  if (dict_mem_) clReleaseMemObject(dict_mem_);
  if (in_mem_) clReleaseMemObject(in_mem_);
  sizes_mem_ = out_mem_ = dict_mem_ = in_mem_ = nullptr;
}

int LzwEncodeOcl::encodeStrips(const uint8_t *rgb, int width, int height,
                               int channels, int rows_per_strip,
                               std::vector<uint32_t> &out_sizes,
                               std::vector<uint8_t> &out_data) {
  out_sizes.clear();
  out_data.clear();
  if (!initOnce()) return -1;
  if (rgb == nullptr || width <= 0 || height <= 0 || channels < 1 ||
      channels > 4 || rows_per_strip <= 0 || height % rows_per_strip != 0) {
    setError("encodeStrips: 参数非法（rows_per_strip 须整除 height）");
    return -2;
  }

  const size_t row_bytes =
      static_cast<size_t>(width) * static_cast<size_t>(channels);
  const size_t strip_bytes = static_cast<size_t>(rows_per_strip) * row_bytes;
  const size_t max_strip = strip_bytes * 3 / 2 + 64;
  const size_t n_strips = static_cast<size_t>(height) / rows_per_strip;
  const size_t block_cap = computeBlockCap(strip_bytes, max_strip);
  const size_t n_blocks = (n_strips + block_cap - 1) / block_cap;

  cl_int err = CL_SUCCESS;
  in_mem_ = clCreateBuffer(ctx_, CL_MEM_READ_ONLY, block_cap * strip_bytes,
                           nullptr, &err);
  if (err != CL_SUCCESS) {
    setError(makeErr("创建输入缓冲失败", err, ""));
    releaseBuffers();
    return -3;
  }
  dict_mem_ = clCreateBuffer(ctx_, CL_MEM_READ_WRITE,
                             block_cap * 4096 * sizeof(cl_uint), nullptr, &err);
  if (err != CL_SUCCESS) {
    setError(makeErr("创建字典缓冲失败", err, ""));
    releaseBuffers();
    return -3;
  }
  out_mem_ = clCreateBuffer(ctx_, CL_MEM_WRITE_ONLY, block_cap * max_strip,
                            nullptr, &err);
  if (err != CL_SUCCESS) {
    setError(makeErr("创建输出缓冲失败", err, ""));
    releaseBuffers();
    return -3;
  }
  sizes_mem_ = clCreateBuffer(ctx_, CL_MEM_WRITE_ONLY,
                              block_cap * sizeof(cl_uint), nullptr, &err);
  if (err != CL_SUCCESS) {
    setError(makeErr("创建尺寸缓冲失败", err, ""));
    releaseBuffers();
    return -3;
  }

  cl_uint sbytes = static_cast<cl_uint>(strip_bytes);
  cl_uint mstrip = static_cast<cl_uint>(max_strip);
  for (size_t b = 0; b < n_blocks; ++b) {
    const size_t bstrips = std::min(block_cap, n_strips - b * block_cap);
    const size_t byte_off = b * block_cap * strip_bytes;
    clEnqueueWriteBuffer(queue_, in_mem_, CL_TRUE, 0, bstrips * strip_bytes,
                         rgb + byte_off, 0, nullptr, nullptr);
    const cl_uint zero32 = 0;
    clEnqueueFillBuffer(queue_, dict_mem_, &zero32, sizeof(zero32), 0,
                        block_cap * 4096 * sizeof(cl_uint), 0, nullptr,
                        nullptr);

    cl_uint ns = static_cast<cl_uint>(bstrips);
    clSetKernelArg(strip_kernel_, 0, sizeof(cl_mem), &in_mem_);
    clSetKernelArg(strip_kernel_, 1, sizeof(cl_mem), &dict_mem_);
    clSetKernelArg(strip_kernel_, 2, sizeof(cl_mem), &out_mem_);
    clSetKernelArg(strip_kernel_, 3, sizeof(cl_mem), &sizes_mem_);
    clSetKernelArg(strip_kernel_, 4, sizeof(cl_uint), &sbytes);
    clSetKernelArg(strip_kernel_, 5, sizeof(cl_uint), &mstrip);
    clSetKernelArg(strip_kernel_, 6, sizeof(cl_uint), &ns);

    const size_t gws = bstrips;
    err = clEnqueueNDRangeKernel(queue_, strip_kernel_, 1, nullptr, &gws,
                                 nullptr, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
      setError(makeErr("enqueue strip kernel 失败", err, ""));
      releaseBuffers();
      return -4;
    }
    err = clFinish(queue_);
    if (err != CL_SUCCESS) {
      setError(makeErr("clFinish 失败", err, ""));
      releaseBuffers();
      return -4;
    }

    std::vector<cl_uint> sz(bstrips);
    if (clEnqueueReadBuffer(queue_, sizes_mem_, CL_TRUE, 0,
                            bstrips * sizeof(cl_uint), sz.data(), 0, nullptr,
                            nullptr) != CL_SUCCESS) {
      setError("读取 strip 尺寸失败");
      releaseBuffers();
      return -5;
    }
    std::vector<uint8_t> all(bstrips * max_strip);
    if (clEnqueueReadBuffer(queue_, out_mem_, CL_TRUE, 0, all.size(),
                            all.data(), 0, nullptr, nullptr) != CL_SUCCESS) {
      setError("读取压缩数据失败");
      releaseBuffers();
      return -5;
    }
    size_t total = 0;
    for (size_t i = 0; i < bstrips; ++i) total += sz[i];
    const size_t woff = out_data.size();
    out_data.resize(woff + total);
    size_t off = 0;
    for (size_t i = 0; i < bstrips; ++i) {
      std::memcpy(out_data.data() + woff + off, all.data() + i * max_strip,
                  sz[i]);
      out_sizes.push_back(sz[i]);
      off += sz[i];
    }
  }
  releaseBuffers();
  return 0;
}

int LzwEncodeOcl::encodeTiles(const uint8_t *rgb, int width, int height,
                              int channels, int tile_size,
                              std::vector<uint32_t> &out_sizes,
                              std::vector<uint8_t> &out_data) {
  out_sizes.clear();
  out_data.clear();
  if (!initOnce()) return -1;
  if (rgb == nullptr || width <= 0 || height <= 0 || channels < 1 ||
      channels > 4 || tile_size <= 0) {
    setError("encodeTiles: 参数非法");
    return -2;
  }

  // tile kernel 需要 -DTILE_H=<n>；缓存按 tile 大小重建。
  if (tile_program_ && built_tile_h_ != tile_size) {
    OclProgram::release(tile_kernel_);
    OclProgram::release(tile_program_);
    tile_kernel_ = nullptr;
    tile_program_ = nullptr;
    built_tile_h_ = -1;
  }
  if (!tile_program_) {
    char opts[128];
    std::snprintf(opts, sizeof(opts), "-cl-std=CL1.2 -DTILE_H=%d", tile_size);
    std::string log;
    cl_int err = OclProgram::build(ctx_, device_id_, lzw_encode_tile_cl_source,
                                   opts, &tile_program_, &log);
    if (err != CL_SUCCESS) {
      setError(makeErr("tile kernel 编译失败", err, log));
      return -3;
    }
    err = OclProgram::createKernel(tile_program_, "lzw_encode_tile",
                                   &tile_kernel_);
    if (err != CL_SUCCESS) {
      setError(makeErr("创建 tile kernel 失败", err, ""));
      return -3;
    }
    built_tile_h_ = tile_size;
  }

  const size_t ts = static_cast<size_t>(tile_size);
  const size_t padded_w = ((static_cast<size_t>(width) + ts - 1) / ts) * ts;
  const size_t padded_h = ((static_cast<size_t>(height) + ts - 1) / ts) * ts;
  const size_t padded_row_bytes = padded_w * static_cast<size_t>(channels);
  const size_t tiles_per_row = padded_w / ts;
  const size_t n_tiles = tiles_per_row * (padded_h / ts);
  const size_t tile_bytes = ts * ts * static_cast<size_t>(channels);
  const size_t max_tile = tile_bytes * 3 / 2 + 64;
  const size_t block_cap = computeBlockCap(tile_bytes, max_tile);
  const size_t n_blocks = (n_tiles + block_cap - 1) / block_cap;

  // 填充图像。
  std::vector<uint8_t> padded(padded_row_bytes * padded_h, 0);
  const size_t src_row_bytes =
      static_cast<size_t>(width) * static_cast<size_t>(channels);
  for (size_t r = 0; r < static_cast<size_t>(height); ++r) {
    std::memcpy(padded.data() + r * padded_row_bytes, rgb + r * src_row_bytes,
                src_row_bytes);
  }

  cl_int err = CL_SUCCESS;
  in_mem_ =
      clCreateBuffer(ctx_, CL_MEM_READ_ONLY, padded.size(), nullptr, &err);
  if (err != CL_SUCCESS) {
    setError(makeErr("创建 tile 输入缓冲失败", err, ""));
    releaseBuffers();
    return -4;
  }
  dict_mem_ = clCreateBuffer(ctx_, CL_MEM_READ_WRITE,
                             block_cap * 4096 * sizeof(cl_uint), nullptr, &err);
  if (err != CL_SUCCESS) {
    setError(makeErr("创建 tile 字典缓冲失败", err, ""));
    releaseBuffers();
    return -4;
  }
  out_mem_ = clCreateBuffer(ctx_, CL_MEM_WRITE_ONLY, block_cap * max_tile,
                            nullptr, &err);
  if (err != CL_SUCCESS) {
    setError(makeErr("创建 tile 输出缓冲失败", err, ""));
    releaseBuffers();
    return -4;
  }
  sizes_mem_ = clCreateBuffer(ctx_, CL_MEM_WRITE_ONLY,
                              block_cap * sizeof(cl_uint), nullptr, &err);
  if (err != CL_SUCCESS) {
    setError(makeErr("创建 tile 尺寸缓冲失败", err, ""));
    releaseBuffers();
    return -4;
  }

  clEnqueueWriteBuffer(queue_, in_mem_, CL_TRUE, 0, padded.size(),
                       padded.data(), 0, nullptr, nullptr);
  clFinish(queue_);

  const cl_uint c_padded_row_bytes = static_cast<cl_uint>(padded_row_bytes);
  const cl_uint c_tile_px_bytes =
      static_cast<cl_uint>(ts * static_cast<size_t>(channels));
  const cl_uint c_tiles_per_row = static_cast<cl_uint>(tiles_per_row);
  const cl_uint c_n_tiles = static_cast<cl_uint>(n_tiles);
  const cl_uint c_max_tile = static_cast<cl_uint>(max_tile);

  for (size_t b = 0; b < n_blocks; ++b) {
    const size_t bt = std::min(block_cap, n_tiles - b * block_cap);
    const cl_uint zero32 = 0;
    clEnqueueFillBuffer(queue_, dict_mem_, &zero32, sizeof(zero32), 0,
                        block_cap * 4096 * sizeof(cl_uint), 0, nullptr,
                        nullptr);

    const cl_uint tile_base = static_cast<cl_uint>(b * block_cap);
    clSetKernelArg(tile_kernel_, 0, sizeof(cl_mem), &in_mem_);
    clSetKernelArg(tile_kernel_, 1, sizeof(cl_mem), &dict_mem_);
    clSetKernelArg(tile_kernel_, 2, sizeof(cl_mem), &out_mem_);
    clSetKernelArg(tile_kernel_, 3, sizeof(cl_mem), &sizes_mem_);
    clSetKernelArg(tile_kernel_, 4, sizeof(cl_uint), &c_padded_row_bytes);
    clSetKernelArg(tile_kernel_, 5, sizeof(cl_uint), &c_tile_px_bytes);
    clSetKernelArg(tile_kernel_, 6, sizeof(cl_uint), &c_tiles_per_row);
    clSetKernelArg(tile_kernel_, 7, sizeof(cl_uint), &c_n_tiles);
    clSetKernelArg(tile_kernel_, 8, sizeof(cl_uint), &c_max_tile);
    clSetKernelArg(tile_kernel_, 9, sizeof(cl_uint), &tile_base);

    const size_t gws = bt;
    err = clEnqueueNDRangeKernel(queue_, tile_kernel_, 1, nullptr, &gws,
                                 nullptr, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
      setError(makeErr("enqueue tile kernel 失败", err, ""));
      releaseBuffers();
      return -5;
    }
    err = clFinish(queue_);
    if (err != CL_SUCCESS) {
      setError(makeErr("clFinish 失败", err, ""));
      releaseBuffers();
      return -5;
    }

    std::vector<cl_uint> sz(bt);
    if (clEnqueueReadBuffer(queue_, sizes_mem_, CL_TRUE, 0,
                            bt * sizeof(cl_uint), sz.data(), 0, nullptr,
                            nullptr) != CL_SUCCESS) {
      setError("读取 tile 尺寸失败");
      releaseBuffers();
      return -6;
    }
    std::vector<uint8_t> all(bt * max_tile);
    if (clEnqueueReadBuffer(queue_, out_mem_, CL_TRUE, 0, all.size(),
                            all.data(), 0, nullptr, nullptr) != CL_SUCCESS) {
      setError("读取 tile 压缩数据失败");
      releaseBuffers();
      return -6;
    }
    size_t total = 0;
    for (size_t i = 0; i < bt; ++i) total += sz[i];
    const size_t woff = out_data.size();
    out_data.resize(woff + total);
    size_t off = 0;
    for (size_t i = 0; i < bt; ++i) {
      std::memcpy(out_data.data() + woff + off, all.data() + i * max_tile,
                  sz[i]);
      out_sizes.push_back(sz[i]);
      off += sz[i];
    }
  }
  releaseBuffers();
  return 0;
}

} // namespace tiff
} // namespace image_codec
