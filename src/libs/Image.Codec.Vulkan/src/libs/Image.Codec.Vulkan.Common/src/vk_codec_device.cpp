// File      : vk_codec_device.cpp
// Brief     : 编解码后端使用的 Vulkan 设备实现
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#include "vk_codec_device.h"

#include <cstring>
#include <vector>

namespace image_codec {
namespace vulkan {

namespace {

// 通过 KHR 入口查询带 pNext 链的物理设备内存属性。
// vkGetPhysicalDeviceMemoryProperties2 是 Vulkan 1.1 核心函数，但我们的 instance
// 按 1.0 创建；KHR 版本由 VK_KHR_get_physical_device_properties2 提供，
// 因此统一走实例级函数指针，避免「1.0 设备上直接调用 1.1 符号」。
PFN_vkGetPhysicalDeviceMemoryProperties2KHR
memoryProperties2Fn(VkInstance instance) {
  return reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2KHR>(
      vkGetInstanceProcAddr(instance,
                            "vkGetPhysicalDeviceMemoryProperties2KHR"));
}

} // namespace

Device::~Device() = default;

bool Device::init(int device_index, std::string *error) {
  vic::Context::Options options;
  options.device_index = device_index;
  options.request_float64 = true;
  options.extra_device_extensions.push_back(
      VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

  const vic::Result result = context_.init(options);
  if (result != vic::Result::Ok) {
    if (error != nullptr) {
      *error = context_.lastError().empty() ? "vulkan: device init failed"
                                            : "vulkan: " + context_.lastError();
    }
    return false;
  }
  // fp64 只「申请」不「强制」：设备支持就启用（指标走 GPU），不支持也让编解码
  // 照常跑——JPEG 的 DCT/颜色变换、PNG 行滤波、TIFF 预测器都是 fp32 运算。
  // 指标自己检查 caps().fp64 并给出明确原因，由上层回退 CPU 指标。

  caps_.name = context_.deviceInfo().name;
  caps_.api_version = context_.deviceInfo().apiVersion;
  caps_.queue_family = context_.queueFamilyIndex();
  caps_.fp64 = context_.float64Enabled();
  caps_.memory_budget_ext =
      context_.deviceExtensionEnabled(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(context_.vkPhysicalDevice(), &props);
  caps_.vendor_id = props.vendorID;
  caps_.device_id = props.deviceID;

  // 总容量与单次分配上限只统计 device-local 堆：host-visible 的暂存堆
  // 不能代表「这张图能不能整块放进显存」。
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(context_.vkPhysicalDevice(), &mp);
  for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
    if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
      continue;
    caps_.total_bytes += static_cast<size_t>(mp.memoryHeaps[i].size);
    if (mp.memoryHeaps[i].size > caps_.max_alloc_bytes) {
      caps_.max_alloc_bytes = static_cast<size_t>(mp.memoryHeaps[i].size);
    }
  }

  ready_ = true;
  return true;
}

void Device::setMemoryLimit(size_t limit_bytes) {
  limit_ = limit_bytes;
  // 降低上限后如果已经超了，下一次 reserve 会立刻拒绝；
  // 已经分配出去的缓冲不会被强制回收（那会让调用方手里出现悬空数据）。
  exhausted_ = limit_ != 0 && used_ > limit_;
}

bool Device::reserve(size_t bytes, std::string *error) {
  if (limit_ == 0) {
    used_ += bytes;
    if (used_ > peak_) peak_ = used_;
    return true;
  }
  if (used_ + bytes > limit_) {
    exhausted_ = true;
    if (error != nullptr) {
      *error = "vulkan: memory budget exceeded: need " + std::to_string(bytes) +
               " B, used " + std::to_string(used_) + " B, limit " +
               std::to_string(limit_) + " B (peak " + std::to_string(peak_) +
               " B)";
    }
    return false;
  }
  used_ += bytes;
  if (used_ > peak_) peak_ = used_;
  return true;
}

void Device::release(size_t bytes) {
  if (bytes >= used_) {
    used_ = 0;
  } else {
    used_ -= bytes;
  }
}

MemoryInfo Device::memoryInfo() const {
  MemoryInfo info;
  info.total_bytes = caps_.total_bytes;
  info.max_alloc_bytes = caps_.max_alloc_bytes;
  info.budget_bytes = limit_;
  info.used_bytes = used_;
  info.peak_bytes = peak_;

  if (caps_.memory_budget_ext) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

    VkPhysicalDeviceMemoryProperties2 mp2{};
    mp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    mp2.pNext = &budget;

    const auto fn = memoryProperties2Fn(context_.vkInstance());
    if (fn != nullptr) {
      fn(context_.vkPhysicalDevice(), &mp2);
      size_t free_total = 0;
      for (uint32_t i = 0; i < mp2.memoryProperties.memoryHeapCount; ++i) {
        if ((mp2.memoryProperties.memoryHeaps[i].flags &
             VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) {
          continue;
        }
        const size_t heap_budget = static_cast<size_t>(budget.heapBudget[i]);
        const size_t heap_usage = static_cast<size_t>(budget.heapUsage[i]);
        if (heap_budget > heap_usage) free_total += heap_budget - heap_usage;
      }
      info.free_bytes = free_total;
    }
  }
  return info;
}

} // namespace vulkan
} // namespace image_codec
