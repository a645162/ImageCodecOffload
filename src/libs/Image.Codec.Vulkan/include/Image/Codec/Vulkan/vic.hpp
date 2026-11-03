/*
 * File: vic.hpp
 * Brief: Vic.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#pragma once
// Image.Codec.Vulkan —— Vulkan compute 结果零拷贝传输到 OpenGL 的 C++ RAII 库。
// 依赖：Vulkan (vcpkg), GL 仅通过本头不暴露；GL 对象以 uint32_t id 传递。
#include <cstdint>
#include <string>
#include <memory>

// vk_platform.h 不自动定义 VK_USE_PLATFORM_WIN32_KHR，需在首个 vulkan.h 包含前
// 显式声明，vulkan.h 才会引入 vulkan_win32.h（Win32 句柄类型）。所有内部头都以
// 本头为公共祖先，在此定义可保证任何 TU 中先于 vulkan.h 生效。
//
// 只在 Windows 上定义：Linux 没有 vulkan_win32.h，无条件定义会让整个仓库在
// Linux 上连 Vulkan 头都包含不进来。编解码（container / shader / 指标）本身与
// 平台无关，只有 external-memory -> GL 那一段是 Windows 专用。
#ifdef _WIN32
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#endif

namespace vic {

enum class Result : int {
  Ok = 0,
  Error = 1,
  UnsupportedExtension = 2, // 互操作扩展缺失（Vulkan 或 GL）→ 应视为 SKIP
  InitFailed = 3,
};

struct DeviceInfo {
  std::string name;
  uint32_t apiVersion = 0;
  bool supportsExternalMemoryWin32 = false; // VK_KHR_external_memory_win32 可用
  bool supportsFloat64 = false; // shaderFloat64 可用（指标累加需要）
};

// ---- 实现分布：Core / GlInterop / Api（见 src/libs/*）----

class Context;          // Vulkan instance/device/queue（Core）
class ExportableBuffer; // 可导出 VkBuffer（Core）
class ExportableImage;  // 可导出 VkImage（Core）
class Compute;          // compute pipeline + dispatch（Core）
class GlInterop;        // GL 扩展加载 + buffer/texture 导入（GlInterop）
class GlImagePipeline;  // 高层：compute→GL 一步到位（Api）

} // namespace vic

// ---- 唯一入口（umbrella）：包含本头即得到全部对外类的完整定义 ----
// 解析依赖 Image.Codec.Vulkan::Api 传播的 PUBLIC include 目录：
//   include/  Core/src  GlInterop/src  Api/src
// 各具体头内部对 <Image/Codec/Vulkan/vic.hpp> 的再包含由 #pragma once 保证安全。
//
// Core（设备/可导出资源/compute）与平台无关；GlInterop / GlImagePipeline 是
// Windows 专用（WGL + VK_KHR_external_memory_win32），在其它平台既不编译也不
// 出现在本头里——否则只是包含 vic.hpp 的编解码模块会被一个用不上的 GL 头拖死。
#include "context.h"
#include "exportable.h"
#include "compute.h"

#ifdef _WIN32
#include "gl_interop.h"
#include "gl_image_pipeline.h"
#endif
