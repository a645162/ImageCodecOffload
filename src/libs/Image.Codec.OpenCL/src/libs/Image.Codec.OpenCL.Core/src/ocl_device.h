/*
 * File: ocl_device.h
 * Brief: Ocl device.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// opencl/ocl_device.h - OpenCL 平台/设备枚举与上下文/命令队列管理。
#pragma once

#include <CL/cl.h>

#include <string>
#include <vector>

namespace image_codec {

struct OclDeviceInfo {
  cl_platform_id platform = nullptr;
  cl_device_id device = nullptr;
  std::string platformName;
  std::string deviceName;
  std::string vendor;
  std::string clVersion;  // 例如 "OpenCL 3.0 ..."
  cl_uint major = 0;
  cl_uint minor = 0;
};

// 持有 context + in-order command queue 的 RAII 设备句柄。
class OclDevice {
public:
  static std::vector<OclDeviceInfo> enumerate();

  OclDevice() = default;
  ~OclDevice();

  OclDevice(const OclDevice &) = delete;
  OclDevice &operator=(const OclDevice &) = delete;

  // 按枚举索引初始化（默认 0）；失败返回 OpenCL 错误码。
  cl_int init(int device_index = 0);
  void release();

  cl_platform_id platform() const { return platform_; }
  cl_device_id device() const { return device_; }
  cl_context context() const { return ctx_; }
  cl_command_queue queue() const { return queue_; }
  bool valid() const { return ctx_ != nullptr; }

private:
  cl_platform_id platform_ = nullptr;
  cl_device_id device_ = nullptr;
  cl_context ctx_ = nullptr;
  cl_command_queue queue_ = nullptr;
};

}  // namespace image_codec
