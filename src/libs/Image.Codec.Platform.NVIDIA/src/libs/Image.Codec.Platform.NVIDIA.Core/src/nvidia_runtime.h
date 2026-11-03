/*
 * File: nvidia_runtime.h
 * Brief: CUDA device, stream and device-memory budget.
 * Author: Haomin Kong
 * Email: konghm@beim.com.cn / a645162@163.com
 * Created: 2026-11-03
 */

// nvidia_runtime.h - CUDA 设备枚举、显式 stream、显存记账与设备缓冲。
//
// 刻意不使用 CUDA Runtime 的「隐式默认 context 懒初始化」：每次编解码都在
// 调用方指定的设备上显式 cudaSetDevice + 显式 stream，避免多卡机器上把活发
// 到错误设备（ImageViewer 会同时枚举独显与核显）。
//
// 显存记账（MemoryBudget）与 Image.Codec.CUDA 的语义一致：reserve 失败即
// 立刻放弃并让上层回退 CPU，绝不把整机拖进 swap。
#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <string>
#include <vector>

namespace image_codec {
namespace nvidia {

struct DeviceInfo {
  int index = 0;
  std::string name;
  int major = 0; // compute capability major
  int minor = 0;
  size_t total_memory = 0;
  bool unified_addressing = false;
  std::string driver_version;
  std::string runtime_version;
};

// 枚举全部 CUDA 设备。没有任何设备时返回空表（不是错误）。
std::vector<DeviceInfo> enumerate_devices();

// 带显式 stream 的设备句柄。
class Device {
public:
  Device() = default;
  ~Device() { release(); }

  Device(const Device &) = delete;
  Device &operator=(const Device &) = delete;

  // 初始化（同进程内可重复调用：同一 index 复用底层 context）。
  // 失败返回 false 并写 err（不打印）。
  bool init(int index, std::string *err);

  void release();

  bool valid() const { return initialized_; }
  int index() const { return info_.index; }
  const DeviceInfo &info() const { return info_; }
  cudaStream_t stream() const { return stream_; }

  // 等待本设备 stream 上的全部工作完成。
  bool synchronize(std::string *err) const;

private:
  DeviceInfo info_;
  cudaStream_t stream_ = nullptr;
  bool initialized_ = false;
};

// 显存预算。limit == 0 表示不限制（但仍统计 used / peak）。
class MemoryBudget {
public:
  explicit MemoryBudget(size_t limit_bytes = 0) : limit_(limit_bytes) {}

  bool limited() const { return limit_ != 0; }
  size_t limit() const { return limit_; }
  size_t used() const { return used_; }
  size_t peak() const { return peak_; }

  // 申请 bytes 的额度；超限时写 err 并返回 false（不分配）。
  bool reserve(size_t bytes, std::string *err);
  void release(size_t bytes);

private:
  size_t limit_ = 0;
  size_t used_ = 0;
  size_t peak_ = 0;
};

// 设备内存 RAII 缓冲。绑定 MemoryBudget 后每次真实分配都先记账、释放时归还，
// 因此「本缓冲占了多少、峰值多少」是真实的，不是估算。
class DeviceBuffer {
public:
  DeviceBuffer() = default;
  ~DeviceBuffer() { reset(); }

  DeviceBuffer(const DeviceBuffer &) = delete;
  DeviceBuffer &operator=(const DeviceBuffer &) = delete;

  DeviceBuffer(DeviceBuffer &&other) noexcept
      : ptr_(other.ptr_), bytes_(other.bytes_), budget_(other.budget_) {
    other.ptr_ = nullptr;
    other.bytes_ = 0;
    other.budget_ = nullptr;
  }
  DeviceBuffer &operator=(DeviceBuffer &&other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.ptr_;
      bytes_ = other.bytes_;
      budget_ = other.budget_;
      other.ptr_ = nullptr;
      other.bytes_ = 0;
      other.budget_ = nullptr;
    }
    return *this;
  }

  // 绑定显存预算（nullptr = 不记账）。必须在任何分配之前调用。
  void set_budget(MemoryBudget *budget) { budget_ = budget; }

  // 确保容量 >= bytes；已足够时不重新分配（热路径不反复 cudaMalloc）。
  bool ensure(size_t bytes, std::string *err);

  void reset();

  void *data() { return ptr_; }
  const void *data() const { return ptr_; }
  size_t bytes() const { return bytes_; }

private:
  void *ptr_ = nullptr;
  size_t bytes_ = 0;
  MemoryBudget *budget_ = nullptr;
};

// 宿主 -> 设备整块拷贝（同步语义由调用方通过 stream 顺序保证）。
bool upload(cudaStream_t stream, void *dst, const void *src, size_t bytes,
            std::string *err);
// 设备 -> 宿主整块拷贝。
bool download(cudaStream_t stream, void *dst, const void *src, size_t bytes,
              std::string *err);

} // namespace nvidia
} // namespace image_codec
