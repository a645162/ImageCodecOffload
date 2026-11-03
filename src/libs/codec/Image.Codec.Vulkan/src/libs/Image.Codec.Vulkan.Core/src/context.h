/*
 * File: context.h
 * Brief: Context.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#pragma once
#include <Image/Codec/Vulkan/vic.hpp>
// 本 SDK 的 vk_platform.h 不再自动定义 VK_USE_PLATFORM_WIN32_KHR，
// 需显式声明后 vulkan.h 才会包含 vulkan_win32.h（提供 Win32 handle 类型）。
// 只有 Windows 有那个头；Linux 上编解码（容器/着色器/指标）走同一份代码，
// 不需要、也不应该被 Win32 类型拖住。
#ifdef _WIN32
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#endif
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace vic {

class Context {
public:
  // init() 的选项。默认值与历史行为一致：枚举全部设备，优先独显。
  struct Options {
    // 设备选择：-1 = 按类型打分自动选（独显 > 核显 > 其它），>=0 = 直接取该下标。
    // 编解码后端需要它：同一台机器上「跑图形的独显」和「有媒体引擎的核显」
    // 未必是同一块卡，调用方要能指定。
    int device_index = -1;
    // 是否要求 fp64（shaderFloat64）。指标累加必须用 double 才能与 CPU 参考
    // 逐位对齐（见 Metrics 模块）。默认只「申请」不「强制」：编解码（JPEG DCT、
    // PNG 行滤波、TIFF 预测器）根本不需要 fp64，把整块卡因为一个可选算子判死刑
    // 会让只有核显的机器彻底用不上本后端。指标自己检查 require_float64。
    bool request_float64 = false;
    // 是否把 fp64 当作硬性要求：为 true 时，缺该特性的设备直接不选（自动选择
    // 时跳过、显式指定时报错），而不是等指标跑到一半才失败。
    bool require_float64 = false;
    // 额外请求的设备扩展（只在设备真的支持时才启用；用
    // deviceExtensionEnabled() 查实际结果）。例如 VK_EXT_memory_budget。
    std::vector<const char *> extra_device_extensions;
  };

  Context() = default;
  ~Context();
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;
  Context(Context &&other) noexcept;
  Context &operator=(Context &&other) noexcept;

  // 注意：这里不写 `= {}` 默认实参。GCC 在 C++17 下不允许在外层类作用域里
  // 用带 NSDMI 的嵌套类型构造默认实参（"default member initializer required
  // before the end of its enclosing class"），故拆成无参重载。
  Result init();
  Result init(const Options &options);

  const DeviceInfo &deviceInfo() const { return info_; }
  VkInstance vkInstance() const { return instance_; }
  VkPhysicalDevice vkPhysicalDevice() const { return physicalDevice_; }
  VkDevice vkDevice() const { return device_; }
  VkQueue vkQueue() const { return queue_; }
  uint32_t queueFamilyIndex() const { return queueFamily_; }
  // 本设备是否真的启用了 fp64（请求了且申请成功才为 true）。
  bool float64Enabled() const { return float64Enabled_; }
#ifdef _WIN32
  PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32Handle() const {
    return vkGetMemoryWin32Handle_;
  }
#endif
  // VK_KHR_external_memory_capabilities / VK_KHR_get_physical_device_properties2：
  // vcpkg 的 vulkan-1.lib 不导出这两个 -KHR 符号，只能经 vkGetInstanceProcAddr 运行时解析。
  PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR
  vkPhysicalDeviceExternalBufferProperties() const {
    return extBufferProps_;
  }
  PFN_vkGetPhysicalDeviceImageFormatProperties2KHR
  vkPhysicalDeviceImageFormatProperties2() const {
    return imageFormatProps2_;
  }

  // 最近一次 init() 失败的原因（成功时清空）。上层要把原因原样报给调用方，
  // 否则「后端不可用」只剩一个枚举值。
  const std::string &lastError() const { return lastError_; }

  // 某个设备扩展在 init() 时是否真的被启用。
  bool deviceExtensionEnabled(const char *name) const;

  Result createExportableMemory(const VkMemoryRequirements &req,
                                VkMemoryPropertyFlags props, bool exportable,
                                VkDeviceMemory &out) const;

private:
  void destroy();

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  DeviceInfo info_;
#ifdef _WIN32
  PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32Handle_ = nullptr;
#endif
  PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR extBufferProps_ = nullptr;
  PFN_vkGetPhysicalDeviceImageFormatProperties2KHR imageFormatProps2_ = nullptr;
  bool enabledExternalWin32_ = false;
  bool float64Enabled_ = false;
  std::string lastError_;
  std::vector<std::string> enabledDeviceExtensions_;
};

} // namespace vic
