/*
 * File: png_filter_ocl.cpp
 * Brief: Png filter ocl.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// png_filter_ocl.cpp - PNG 行滤波 OpenCL 实现。
#include "png_filter_ocl.h"

#include <limits>

#include "ocl_program.h"

namespace image_codec {
namespace ocl {
namespace png {

namespace {

// 裁剪到 [32, 256]：kernel 的 __local 归约数组按 256 固定。
int clampThreads(int threads) {
  int t = threads > 0 ? threads : 256;
  if (t < 32) t = 32;
  if (t > 256) t = 256;
  return t;
}

} // namespace

PngFilterOcl::PngFilterOcl(int device_index) : device_index_(device_index) {}

PngFilterOcl::~PngFilterOcl() {
  if (k_rows_ != nullptr) OclProgram::release(k_rows_);
  if (k_sums_ != nullptr) OclProgram::release(k_sums_);
  if (prog_ != nullptr) OclProgram::release(prog_);
}

bool PngFilterOcl::initOnce() {
  if (init_done_) return prog_ != nullptr;

  init_done_ = true;
  const cl_int rc = dev_.init(device_index_);
  if (rc != CL_SUCCESS) {
    err_ = std::string("PNG filter: OpenCL init failed: ") +
           OclProgram::errorName(rc);
    return false;
  }

  std::string log;
  cl_int e = OclProgram::build(dev_.context(), dev_.device(), kPngFilterSource,
                               "-cl-std=CL1.2", &prog_, &log);
  if (e != CL_SUCCESS) {
    err_ = "PNG filter: kernel build failed: " + log;
    prog_ = nullptr;
    return false;
  }
  e = OclProgram::createKernel(prog_, "png_filter_rows", &k_rows_);
  if (e != CL_SUCCESS) {
    err_ = "PNG filter: createKernel(png_filter_rows) failed";
    return false;
  }
  e = OclProgram::createKernel(prog_, "png_filter_sums", &k_sums_);
  if (e != CL_SUCCESS) {
    err_ = "PNG filter: createKernel(png_filter_sums) failed";
    return false;
  }
  return true;
}

bool PngFilterOcl::buildFilteredStream(const ImageBuffer &img, int filter_type,
                                       std::vector<uint8_t> *out,
                                       std::vector<uint8_t> *filters_out) {
  if (out == nullptr) {
    err_ = "PNG filter: null output";
    return false;
  }
  out->clear();
  if (filters_out != nullptr) filters_out->clear();
  if (!img.valid()) {
    err_ = "PNG filter: invalid image buffer";
    return false;
  }
  if (filter_type < -1 || filter_type > 4) {
    err_ = "PNG filter: filter_type out of range";
    return false;
  }
  if (!initOnce()) {
    return false;
  }

  const size_t row_bytes = img.stride();
  const size_t total = (row_bytes + 1) * static_cast<size_t>(img.height);
  const cl_int width = img.width;
  const cl_int height = img.height;
  const cl_int channels = img.channels;
  cl_int e = CL_SUCCESS;

  cl_mem src = clCreateBuffer(
      dev_.context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, img.data.size(),
      const_cast<uint8_t *>(img.data.data()), &e);
  if (e != CL_SUCCESS) {
    err_ = std::string("PNG filter: clCreateBuffer(src): ") +
           OclProgram::errorName(e);
    return false;
  }

  std::vector<uint8_t> filters;
  cl_mem types = nullptr;
  if (filter_type < 0) {
    // 1) GPU 统计 5 种滤波器的残差绝对值和。
    const size_t sums_bytes =
        static_cast<size_t>(height) * 5 * sizeof(cl_ulong);
    cl_mem sums = clCreateBuffer(dev_.context(), CL_MEM_WRITE_ONLY, sums_bytes,
                                 nullptr, &e);
    if (e != CL_SUCCESS) {
      err_ = std::string("PNG filter: clCreateBuffer(sums): ") +
             OclProgram::errorName(e);
      clReleaseMemObject(src);
      return false;
    }

    const size_t local = static_cast<size_t>(clampThreads(block_threads_));
    const size_t global = local * static_cast<size_t>(height);
    clSetKernelArg(k_sums_, 0, sizeof(cl_mem), &src);
    clSetKernelArg(k_sums_, 1, sizeof(cl_mem), &sums);
    clSetKernelArg(k_sums_, 2, sizeof(cl_int), &width);
    clSetKernelArg(k_sums_, 3, sizeof(cl_int), &channels);
    e = clEnqueueNDRangeKernel(dev_.queue(), k_sums_, 1, nullptr, &global,
                               &local, 0, nullptr, nullptr);
    if (e != CL_SUCCESS) {
      err_ = std::string("PNG filter: png_filter_sums enqueue: ") +
             OclProgram::errorName(e);
      clReleaseMemObject(src);
      clReleaseMemObject(sums);
      return false;
    }

    std::vector<cl_ulong> host_sums(static_cast<size_t>(height) * 5);
    e = clEnqueueReadBuffer(dev_.queue(), sums, CL_TRUE, 0, sums_bytes,
                            host_sums.data(), 0, nullptr, nullptr);
    clReleaseMemObject(sums);
    if (e != CL_SUCCESS) {
      err_ = std::string("PNG filter: read sums: ") + OclProgram::errorName(e);
      clReleaseMemObject(src);
      return false;
    }

    // 2) 逐行选最小（height 行，纯标量，代价可忽略）。
    filters.resize(static_cast<size_t>(height));
    for (cl_int y = 0; y < height; ++y) {
      cl_ulong best = std::numeric_limits<cl_ulong>::max();
      int best_f = 0;
      for (int f = 0; f < 5; ++f) {
        const cl_ulong v = host_sums[static_cast<size_t>(y) * 5 + f];
        if (v < best) {
          best = v;
          best_f = f;
        }
      }
      filters[static_cast<size_t>(y)] = static_cast<uint8_t>(best_f);
    }

    types =
        clCreateBuffer(dev_.context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                       filters.size(), filters.data(), &e);
    if (e != CL_SUCCESS) {
      err_ = std::string("PNG filter: clCreateBuffer(types): ") +
             OclProgram::errorName(e);
      clReleaseMemObject(src);
      return false;
    }
  }

  if (types == nullptr) {
    // 固定 filter：仍然给 kernel 一份 types 缓冲（全行同值）。这样 kernel 里
    // 不需要区分「指针是否为 NULL」这条与实现相关的路径。
    filters.assign(static_cast<size_t>(height),
                   static_cast<uint8_t>(filter_type));
    types =
        clCreateBuffer(dev_.context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                       filters.size(), filters.data(), &e);
    if (e != CL_SUCCESS) {
      err_ = std::string("PNG filter: clCreateBuffer(types): ") +
             OclProgram::errorName(e);
      clReleaseMemObject(src);
      types = nullptr;
      return false;
    }
  }

  cl_mem dst =
      clCreateBuffer(dev_.context(), CL_MEM_WRITE_ONLY, total, nullptr, &e);
  if (e != CL_SUCCESS) {
    err_ = std::string("PNG filter: clCreateBuffer(dst): ") +
           OclProgram::errorName(e);
    clReleaseMemObject(src);
    clReleaseMemObject(types);
    return false;
  }

  const cl_int uniform = filter_type;

  clSetKernelArg(k_rows_, 0, sizeof(cl_mem), &src);
  clSetKernelArg(k_rows_, 1, sizeof(cl_mem), &dst);
  clSetKernelArg(k_rows_, 2, sizeof(cl_mem), &types);
  clSetKernelArg(k_rows_, 3, sizeof(cl_int), &width);
  clSetKernelArg(k_rows_, 4, sizeof(cl_int), &channels);
  clSetKernelArg(k_rows_, 5, sizeof(cl_int), &uniform);

  const size_t gdim[2] = {row_bytes, static_cast<size_t>(height)};
  e = clEnqueueNDRangeKernel(dev_.queue(), k_rows_, 2, nullptr, gdim, nullptr,
                             0, nullptr, nullptr);
  if (e != CL_SUCCESS) {
    err_ = std::string("PNG filter: png_filter_rows enqueue: ") +
           OclProgram::errorName(e);
    clReleaseMemObject(src);
    clReleaseMemObject(dst);
    if (types != nullptr) clReleaseMemObject(types);
    return false;
  }

  out->resize(total);
  e = clEnqueueReadBuffer(dev_.queue(), dst, CL_TRUE, 0, total, out->data(), 0,
                          nullptr, nullptr);
  clReleaseMemObject(src);
  clReleaseMemObject(dst);
  if (types != nullptr) clReleaseMemObject(types);
  if (e != CL_SUCCESS) {
    err_ = std::string("PNG filter: read filtered stream: ") +
           OclProgram::errorName(e);
    out->clear();
    return false;
  }

  if (filters_out != nullptr) {
    *filters_out = std::move(filters);
  }
  return true;
}

} // namespace png
} // namespace ocl
} // namespace image_codec
