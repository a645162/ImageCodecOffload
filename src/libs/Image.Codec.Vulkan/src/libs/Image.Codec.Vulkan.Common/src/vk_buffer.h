// File      : vk_buffer.h
// Brief     : host-visible Vulkan 存储缓冲（RAII + 显存预算记账）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 编解码路径的缓冲全部是 host-visible + host-coherent：
//   * 输入是宿主内存里刚解码/刚读盘的一张图，本来就要过一次 PCIe；
//   * 结果要回读到宿主内存（容器编码、Huffman、zlib 都在 CPU 上收尾）。
// 与其为此引入 staging buffer + copy，不如直接用一致内存：少一次拷贝，
// 少一份显存，也少一类「忘了 barrier」的错。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// 先包含 vk_codec_device.h：它经由 vic.hpp 定义 VK_USE_PLATFORM_WIN32_KHR，
// 必须早于 <vulkan/vulkan.h>，否则 vulkan_win32.h 不会被引入。
#include "vk_codec_device.h"

#include <vulkan/vulkan.h>

namespace image_codec {
namespace vulkan {

class Buffer {
public:
  // 默认构造 = 空缓冲（ok() 为 false）。用于「先声明成员、稍后 ensure()」
  // 的场景，避免调用方被迫用指针或 optional 包一层。
  Buffer() = default;
  // 设备可以传 nullptr：此时只做裸分配，不记账（用于临时缓冲/自测）。
  // 构造失败用 ok() 判断；失败原因见 error()。
  Buffer(Device *device, size_t size);
  ~Buffer();
  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;
  Buffer(Buffer &&other) noexcept;
  Buffer &operator=(Buffer &&other) noexcept;

  bool ok() const { return buffer_ != VK_NULL_HANDLE && mapped_ != nullptr; }
  VkBuffer vkBuffer() const { return buffer_; }
  size_t size() const { return size_; }
  void *mapped() const { return mapped_; }
  const std::string &error() const { return error_; }

  // 上传/回读一段字节（offset + bytes <= size）。
  bool upload(const void *data, size_t bytes, size_t offset = 0);
  bool download(void *out, size_t bytes, size_t offset = 0) const;
  // 整体清零（累加器每轮统计前要清零）。
  void zero();

  // 确保容量 >= needed，不够就重建。返回是否可用。
  bool ensure(size_t needed);

private:
  void destroy();

  Device *device_ = nullptr;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  void *mapped_ = nullptr;
  size_t size_ = 0;
  size_t added_ = 0; // 记账时预留成功、但可能还没 release 的字节数
  bool owns_memory_ = false;
  std::string error_;
};

} // namespace vulkan
} // namespace image_codec
