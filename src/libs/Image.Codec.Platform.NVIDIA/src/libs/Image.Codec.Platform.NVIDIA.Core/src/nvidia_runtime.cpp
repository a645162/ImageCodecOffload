/*
 * File: nvidia_runtime.cpp
 * Brief: CUDA device, stream and device-memory budget.
 * Author: Haomin Kong
 * Email: konghm@beim.com.cn / a645162@163.com
 * Created: 2026-11-03
 */

#include "nvidia_runtime.h"

#include "nvidia_error.h"

namespace image_codec {
namespace nvidia {

std::vector<DeviceInfo> enumerate_devices() {
  std::vector<DeviceInfo> result;
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) {
    // 没有设备（或驱动缺失）不是错误：上层据此走 CPU 回退。
    (void)cudaGetLastError();
    return result;
  }
  result.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, i) != cudaSuccess) {
      (void)cudaGetLastError();
      continue;
    }
    DeviceInfo info;
    info.index = i;
    info.name = prop.name;
    info.major = prop.major;
    info.minor = prop.minor;
    info.total_memory = prop.totalGlobalMem;
    info.unified_addressing = prop.unifiedAddressing != 0;
    int driver = 0;
    int runtime = 0;
    if (cudaDriverGetVersion(&driver) == cudaSuccess) {
      info.driver_version = std::to_string(driver / 1000) + "." +
                            std::to_string((driver % 1000) / 10);
    }
    if (cudaRuntimeGetVersion(&runtime) == cudaSuccess) {
      info.runtime_version = std::to_string(runtime / 1000) + "." +
                             std::to_string((runtime % 1000) / 10);
    }
    result.push_back(std::move(info));
  }
  return result;
}

bool Device::init(int index, std::string *err) {
  if (initialized_ && info_.index == index) {
    return true;
  }
  release();
  if (!check_cuda(cudaSetDevice(index), "cudaSetDevice", err)) {
    return false;
  }
  cudaDeviceProp prop{};
  if (!check_cuda(cudaGetDeviceProperties(&prop, index),
                  "cudaGetDeviceProperties", err)) {
    return false;
  }
  info_.index = index;
  info_.name = prop.name;
  info_.major = prop.major;
  info_.minor = prop.minor;
  info_.total_memory = prop.totalGlobalMem;
  info_.unified_addressing = prop.unifiedAddressing != 0;
  int driver = 0;
  int runtime = 0;
  if (cudaDriverGetVersion(&driver) == cudaSuccess) {
    info_.driver_version = std::to_string(driver / 1000) + "." +
                           std::to_string((driver % 1000) / 10);
  }
  if (cudaRuntimeGetVersion(&runtime) == cudaSuccess) {
    info_.runtime_version = std::to_string(runtime / 1000) + "." +
                            std::to_string((runtime % 1000) / 10);
  }
  // 非阻塞流：与 legacy default stream 之间没有隐式顺序，因此本模块内所有
  // 拷贝都必须显式走这个 stream（漏一处就会读到旧数据）。
  if (!check_cuda(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking),
                  "cudaStreamCreateWithFlags", err)) {
    stream_ = nullptr;
    return false;
  }
  initialized_ = true;
  return true;
}

void Device::release() {
  if (stream_ != nullptr) {
    (void)cudaStreamDestroy(stream_);
    stream_ = nullptr;
  }
  initialized_ = false;
}

bool Device::synchronize(std::string *err) const {
  if (stream_ == nullptr) {
    if (err != nullptr) *err = "CUDA stream is not initialized";
    return false;
  }
  return check_cuda(cudaStreamSynchronize(stream_), "cudaStreamSynchronize",
                    err);
}

bool MemoryBudget::reserve(size_t bytes, std::string *err) {
  if (limited() && used_ + bytes > limit_) {
    if (err != nullptr) {
      *err = "device memory budget exceeded: request " + std::to_string(bytes) +
             " B, used " + std::to_string(used_) + " B, limit " +
             std::to_string(limit_) + " B";
    }
    return false;
  }
  used_ += bytes;
  if (used_ > peak_) {
    peak_ = used_;
  }
  return true;
}

void MemoryBudget::release(size_t bytes) {
  used_ = bytes >= used_ ? 0 : (used_ - bytes);
}

bool DeviceBuffer::ensure(size_t bytes, std::string *err) {
  if (bytes <= bytes_ && ptr_ != nullptr) {
    return true;
  }
  if (bytes == 0) {
    return true;
  }
  if (budget_ != nullptr && !budget_->reserve(bytes, err)) {
    return false;
  }
  void *p = nullptr;
  if (!check_cuda(cudaMalloc(&p, bytes), "cudaMalloc", err)) {
    if (budget_ != nullptr) budget_->release(bytes);
    return false;
  }
  reset();
  ptr_ = p;
  bytes_ = bytes;
  return true;
}

void DeviceBuffer::reset() {
  if (ptr_ != nullptr) {
    (void)cudaFree(ptr_);
    if (budget_ != nullptr) budget_->release(bytes_);
    ptr_ = nullptr;
  }
  bytes_ = 0;
}

bool upload(cudaStream_t stream, void *dst, const void *src, size_t bytes,
            std::string *err) {
  if (bytes == 0) {
    return true;
  }
  return check_cuda(
      cudaMemcpyAsync(dst, src, bytes, cudaMemcpyHostToDevice, stream),
      "cudaMemcpyAsync H2D", err);
}

bool download(cudaStream_t stream, void *dst, const void *src, size_t bytes,
              std::string *err) {
  if (bytes == 0) {
    return true;
  }
  return check_cuda(
      cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDeviceToHost, stream),
      "cudaMemcpyAsync D2H", err);
}

} // namespace nvidia
} // namespace image_codec
