// File      : vk_buffer.cpp
// Brief     : host-visible Vulkan 存储缓冲实现
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#include "vk_buffer.h"

#include <cstring>
#include <utility>

namespace image_codec {
namespace vulkan {

namespace {

// Vulkan 要求 size > 0；同时让对齐不成为调用方的负担（最小 16 字节）。
size_t alignSize(size_t size) {
  const size_t minimum = 16;
  if (size < minimum) return minimum;
  return (size + minimum - 1) / minimum * minimum;
}

} // namespace

Buffer::Buffer(Device *device, size_t size)
    : device_(device), size_(alignSize(size)) {
  if (device_ == nullptr) {
    error_ = "vulkan: buffer created without a device";
    return;
  }
  if (!device_->reserve(size_, &error_)) return;
  added_ = size_;

  const VkDevice vk_device = device_->context().vkDevice();

  VkBufferCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  ci.size = size_;
  ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
             VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(vk_device, &ci, nullptr, &buffer_) != VK_SUCCESS) {
    error_ = "vulkan: vkCreateBuffer failed";
    if (device_ != nullptr) device_->release(added_);
    added_ = 0;
    return;
  }

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(vk_device, buffer_, &req);

  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(device_->context().vkPhysicalDevice(),
                                      &mp);

  const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  int best = -1;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) == 0) continue;
    if ((mp.memoryTypes[i].propertyFlags & want) != want) continue;
    best = static_cast<int>(i);
    break;
  }
  if (best < 0) {
    error_ = "vulkan: no host-visible coherent memory type for buffer";
    vkDestroyBuffer(vk_device, buffer_, nullptr);
    buffer_ = VK_NULL_HANDLE;
    if (device_ != nullptr) device_->release(added_);
    added_ = 0;
    return;
  }

  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = static_cast<uint32_t>(best);
  if (vkAllocateMemory(vk_device, &ai, nullptr, &memory_) != VK_SUCCESS) {
    error_ = "vulkan: vkAllocateMemory failed";
    vkDestroyBuffer(vk_device, buffer_, nullptr);
    buffer_ = VK_NULL_HANDLE;
    if (device_ != nullptr) device_->release(added_);
    added_ = 0;
    return;
  }

  if (vkBindBufferMemory(vk_device, buffer_, memory_, 0) != VK_SUCCESS) {
    error_ = "vulkan: vkBindBufferMemory failed";
    destroy();
    return;
  }

  if (vkMapMemory(vk_device, memory_, 0, size_, 0, &mapped_) != VK_SUCCESS) {
    error_ = "vulkan: vkMapMemory failed";
    mapped_ = nullptr;
    destroy();
    return;
  }
}

Buffer::~Buffer() { destroy(); }

Buffer::Buffer(Buffer &&other) noexcept { *this = std::move(other); }

Buffer &Buffer::operator=(Buffer &&other) noexcept {
  if (this != &other) {
    destroy();
    device_ = other.device_;
    buffer_ = other.buffer_;
    memory_ = other.memory_;
    mapped_ = other.mapped_;
    size_ = other.size_;
    added_ = other.added_;
    error_ = std::move(other.error_);
    other.device_ = nullptr;
    other.buffer_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.mapped_ = nullptr;
    other.size_ = 0;
    other.added_ = 0;
  }
  return *this;
}

bool Buffer::upload(const void *data, size_t bytes, size_t offset) {
  if (!ok() || data == nullptr) return false;
  if (offset > size_ || bytes > size_ - offset) return false;
  std::memcpy(static_cast<uint8_t *>(mapped_) + offset, data, bytes);
  return true;
}

bool Buffer::download(void *out, size_t bytes, size_t offset) const {
  if (!ok() || out == nullptr) return false;
  if (offset > size_ || bytes > size_ - offset) return false;
  std::memcpy(out, static_cast<const uint8_t *>(mapped_) + offset, bytes);
  return true;
}

void Buffer::zero() {
  if (ok()) std::memset(mapped_, 0, size_);
}

bool Buffer::ensure(size_t needed) {
  if (ok() && size_ >= needed) return true;
  *this = Buffer(device_, needed);
  return ok();
}

void Buffer::destroy() {
  if (device_ == nullptr) return;
  const VkDevice vk_device = device_->context().vkDevice();
  if (mapped_ != nullptr && memory_ != VK_NULL_HANDLE) {
    vkUnmapMemory(vk_device, memory_);
    mapped_ = nullptr;
  }
  if (memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(vk_device, memory_, nullptr);
    memory_ = VK_NULL_HANDLE;
  }
  if (buffer_ != VK_NULL_HANDLE) {
    vkDestroyBuffer(vk_device, buffer_, nullptr);
    buffer_ = VK_NULL_HANDLE;
  }
  if (added_ != 0) {
    device_->release(added_);
    added_ = 0;
  }
}

} // namespace vulkan
} // namespace image_codec
