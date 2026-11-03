/*
 * File: exportable.h
 * Brief: Exportable.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#pragma once
#include "context.h"
#include <vulkan/vulkan.h>

namespace vic {

// 注：ctx_ 用指针（而非引用）承载 Context，以便 move 赋值时重新绑定上下文；
// Context 引用不可重绑，引用成员上的 ctx_ = other.ctx_ 会触发 Context 拷贝赋值（已删除）。
class ExportableBuffer {
public:
  ExportableBuffer(Context& ctx, VkDeviceSize size);
  ~ExportableBuffer();
  ExportableBuffer(const ExportableBuffer&) = delete;
  ExportableBuffer& operator=(const ExportableBuffer&) = delete;
  ExportableBuffer(ExportableBuffer&& other) noexcept;
  ExportableBuffer& operator=(ExportableBuffer&& other) noexcept;

  VkBuffer vkBuffer() const { return buffer_; }
  void* exportHandle();
  VkDeviceSize size() const { return size_; }

private:
  void destroy();
  Context* ctx_;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  VkDeviceSize size_ = 0;
  void* exportedHandle_ = nullptr;
};

class ExportableImage {
public:
  ExportableImage(Context& ctx, uint32_t width, uint32_t height, VkFormat format);
  ~ExportableImage();
  ExportableImage(const ExportableImage&) = delete;
  ExportableImage& operator=(const ExportableImage&) = delete;
  ExportableImage(ExportableImage&& other) noexcept;
  ExportableImage& operator=(ExportableImage&& other) noexcept;

  VkImage vkImage() const { return image_; }
  VkImageView vkView() const { return view_; }
  void* exportHandle();
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  VkDeviceSize memorySize() const { return size_; }  // VkImageMemoryRequirements::size

private:
  void destroy();
  Context* ctx_;
  VkImage image_ = VK_NULL_HANDLE;
  VkImageView view_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  VkDeviceSize size_ = 0;
  void* exportedHandle_ = nullptr;
};

}  // namespace vic
