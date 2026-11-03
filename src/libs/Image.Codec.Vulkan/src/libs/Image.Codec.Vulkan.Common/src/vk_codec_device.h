// File      : vk_codec_device.h
// Brief     : 编解码后端使用的 Vulkan 设备（含显存预算记账）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 与 vic::Context 的分工：Context 只负责「建 instance/device/queue」，是给
// GL 互操作用的通用层；本类在它之上补三件编解码真正需要的事：
//   1. 设备支持 fp64（shaderFloat64）时启用它：指标累加必须 double 才能与
//      CPU 参考逐位对齐。设备没有这个特性不影响编解码，指标自己会明确失败
//      并让上层回退 CPU——绝不退化成 float 计算出一个「看着差不多」的数字；

//   2. 显存预算：每次分配先记账，超上限立刻失败并给出原因，绝不把整机拖进
//      swap。CUDA 后端的 MemoryBudget 就是同一个思路，两边行为要对得上；
//   3. 把「总显存 / 空闲显存 / 单次分配上限」问清楚，供上层在创建句柄之前
//      判断「这台机器吃不吃得下这张图」。
//
// 空闲显存的来源是 VK_EXT_memory_budget（设备扩展，可用则启用）：
// 它给出 heapBudget/heapUsage，比「heapSize - 我们自己用了多少」真实得多
// （其它进程、驱动内部占用都算在内）。扩展不可用时 free_bytes 返回 0，
// 表示「驱动不提供」，而不是「没有空闲显存」。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <Image/Codec/Vulkan/vic.hpp>

namespace image_codec {
namespace vulkan {

// 设备能力快照（创建句柄时确定，之后不再变）。
struct DeviceCaps {
  std::string name;
  uint32_t api_version = 0;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  uint32_t queue_family = 0;
  bool fp64 = false;              // shaderFloat64 已启用
  bool memory_budget_ext = false; // VK_EXT_memory_budget 已启用
  size_t total_bytes = 0;         // 所有 device-local 堆的总容量
  size_t max_alloc_bytes = 0;     // 单次分配上限（最大的 device-local 堆）
};

// 显存信息（字节）。字段语义与 Image.Codec.CUDA 的 memory_info 对齐。
struct MemoryInfo {
  size_t total_bytes = 0;
  size_t free_bytes = 0; // 驱动视角的空闲（0 = 后端/驱动不提供）
  size_t max_alloc_bytes = 0;
  size_t budget_bytes = 0; // 本句柄上限（0 = 不限制）
  size_t used_bytes = 0;   // 本句柄当前占用
  size_t peak_bytes = 0;   // 本句柄历史峰值
};

class Device {
public:
  Device() = default;
  ~Device();
  Device(const Device &) = delete;
  Device &operator=(const Device &) = delete;

  // 建 instance/device/queue。device_index < 0 = 自动选（独显优先）。
  // 设备支持 fp64 时启用（指标要用 double 累加）；不支持也能建成——编解码本身
  // 不需要 fp64，指标调用会自己检查 caps().fp64 并返回明确原因。
  bool init(int device_index, std::string *error);

  bool ready() const { return ready_; }
  const DeviceCaps &caps() const { return caps_; }
  vic::Context &context() { return context_; }
  const vic::Context &context() const { return context_; }

  // ---- 显存预算 ----
  // limit_bytes == 0 表示不限制（仍受设备真实空闲显存约束）。
  void setMemoryLimit(size_t limit_bytes);
  // 预留/归还。reserve 失败时 error 说明「需要多少 / 上限多少 / 峰值多少」。
  bool reserve(size_t bytes, std::string *error);
  void release(size_t bytes);
  // 上一次 reserve 是否因超预算失败（上层据此把失败分类成 OutOfMemory）。
  bool exhausted() const { return exhausted_; }
  bool takeExhausted() {
    const bool was = exhausted_;
    exhausted_ = false;
    return was;
  }

  MemoryInfo memoryInfo() const;

private:
  vic::Context context_;
  DeviceCaps caps_;
  size_t limit_ = 0;
  size_t used_ = 0;
  size_t peak_ = 0;
  bool exhausted_ = false;
  bool ready_ = false;
};

} // namespace vulkan
} // namespace image_codec
