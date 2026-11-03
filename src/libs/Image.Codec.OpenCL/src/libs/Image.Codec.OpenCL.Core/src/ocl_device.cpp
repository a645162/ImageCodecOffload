/*
 * File: ocl_device.cpp
 * Brief: Ocl device.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#include "ocl_device.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace image_codec {

std::vector<OclDeviceInfo> OclDevice::enumerate() {
  std::vector<OclDeviceInfo> out;
  cl_uint numPlatforms = 0;
  if (clGetPlatformIDs(0, nullptr, &numPlatforms) != CL_SUCCESS || numPlatforms == 0) {
    return out;
  }
  std::vector<cl_platform_id> platforms(numPlatforms);
  if (clGetPlatformIDs(numPlatforms, platforms.data(), nullptr) != CL_SUCCESS) {
    return out;
  }
  for (cl_platform_id pf : platforms) {
    cl_uint numDevices = 0;
    if (clGetDeviceIDs(pf, CL_DEVICE_TYPE_ALL, 0, nullptr, &numDevices) != CL_SUCCESS ||
        numDevices == 0) {
      continue;
    }
    std::vector<cl_device_id> devices(numDevices);
    if (clGetDeviceIDs(pf, CL_DEVICE_TYPE_ALL, numDevices, devices.data(), nullptr) !=
        CL_SUCCESS) {
      continue;
    }
    for (cl_device_id dev : devices) {
      OclDeviceInfo info;
      info.platform = pf;
      info.device = dev;
      char buf[256] = {0};
      if (clGetPlatformInfo(pf, CL_PLATFORM_NAME, sizeof(buf), buf, nullptr) == CL_SUCCESS) {
        info.platformName = buf;
      }
      if (clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(buf), buf, nullptr) == CL_SUCCESS) {
        info.deviceName = buf;
      }
      if (clGetDeviceInfo(dev, CL_DEVICE_VENDOR, sizeof(buf), buf, nullptr) == CL_SUCCESS) {
        info.vendor = buf;
      }
      if (clGetDeviceInfo(dev, CL_DEVICE_VERSION, sizeof(buf), buf, nullptr) == CL_SUCCESS) {
        info.clVersion = buf;
      }
      unsigned major = 0, minor = 0;
      std::sscanf(info.clVersion.c_str(), "OpenCL %u.%u", &major, &minor);
      info.major = major;
      info.minor = minor;
      out.push_back(std::move(info));
    }
  }
  return out;
}

OclDevice::~OclDevice() { release(); }

cl_int OclDevice::init(int device_index) {
  release();
  const std::vector<OclDeviceInfo> devs = enumerate();
  if (device_index < 0 || static_cast<size_t>(device_index) >= devs.size()) {
    return CL_DEVICE_NOT_FOUND;
  }
  platform_ = devs[static_cast<size_t>(device_index)].platform;
  device_ = devs[static_cast<size_t>(device_index)].device;

  cl_int err = CL_SUCCESS;
  cl_context_properties props[] = {
      CL_CONTEXT_PLATFORM,
      reinterpret_cast<cl_context_properties>(platform_),
      0};
  ctx_ = clCreateContext(props, 1, &device_, nullptr, nullptr, &err);
  if (err != CL_SUCCESS || ctx_ == nullptr) {
    ctx_ = nullptr;
    device_ = nullptr;
    platform_ = nullptr;
    return err;
  }
  queue_ = clCreateCommandQueue(ctx_, device_, 0, &err);
  if (err != CL_SUCCESS || queue_ == nullptr) {
    queue_ = nullptr;
    release();
    return err;
  }
  return CL_SUCCESS;
}

void OclDevice::release() {
  if (queue_ != nullptr) {
    clReleaseCommandQueue(queue_);
    queue_ = nullptr;
  }
  if (ctx_ != nullptr) {
    clReleaseContext(ctx_);
    ctx_ = nullptr;
  }
  // device/platform 引用由 context 持有，不单独 release，避免引用计数错乱。
  device_ = nullptr;
  platform_ = nullptr;
}

}  // namespace image_codec
