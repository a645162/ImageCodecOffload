/*
 * File: exportable.cpp
 * Brief: Exportable.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#include "exportable.h"
#ifdef _WIN32
#include <windows.h> // CloseHandle（exportedHandle_ 为 OPAQUE_WIN32 句柄）
#endif

namespace vic {
namespace {

// VUID-09860：VkExportMemoryAllocateInfo::handleTypes 必须被报告为
// "supported and compatible"（VkExternalBufferProperties /
// VkExternalImageFormatProperties）。Vulkan 1.4 起 VkExternalMemoryProperties
// 不再携带 memoryTypeBits —— 资源以 handleTypes 创建后，实现会把不支持的
// memory type 从 vkGet(Buffer|Image)MemoryRequirements 的 memoryTypeBits 中
// 过滤掉，因此类型选择在 createExportableMemory 中只需基于 req.memoryTypeBits。
// 这里按句柄类型做资源级导出能力验证（EXPORTABLE_BIT），决定是否可链入导出信息。
bool bufferExportable(Context &ctx, VkBufferUsageFlags usage) {
#ifdef _WIN32
  VkPhysicalDeviceExternalBufferInfo ebi{};
  ebi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
  ebi.usage = usage;
  ebi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  VkExternalBufferProperties ep{};
  ep.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
  auto fn = ctx.vkPhysicalDeviceExternalBufferProperties();
  if (!fn) return false;
  fn(ctx.vkPhysicalDevice(), &ebi, &ep);
  return (ep.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
#else
  // 非 Windows 没有 VK_KHR_external_memory_win32：本平台不提供 GL 互操作，
  // 资源按普通设备内存创建（不链 VkExportMemoryAllocateInfo）。
  (void)ctx;
  (void)usage;
  return false;
#endif
}

bool imageExportable(Context &ctx, const VkImageCreateInfo &ici) {
#ifdef _WIN32
  VkPhysicalDeviceExternalImageFormatInfo extInfo{};
  extInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  extInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  VkPhysicalDeviceImageFormatInfo2 fi2{};
  fi2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
  fi2.pNext = &extInfo;
  fi2.format = ici.format;
  fi2.type = ici.imageType;
  fi2.tiling = ici.tiling;
  fi2.usage = ici.usage;
  fi2.flags = ici.flags;
  VkExternalImageFormatProperties extProps{};
  extProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 ip2{};
  ip2.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
  ip2.pNext = &extProps;
  auto fn = ctx.vkPhysicalDeviceImageFormatProperties2();
  if (!fn || fn(ctx.vkPhysicalDevice(), &fi2, &ip2) != VK_SUCCESS) return false;
  return (extProps.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
#else
  (void)ctx;
  (void)ici;
  return false;
#endif
}

} // namespace

ExportableBuffer::ExportableBuffer(Context &ctx, VkDeviceSize size)
    : ctx_(&ctx), size_(size) {
#ifdef _WIN32
  VkExternalMemoryBufferCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#endif

  VkBufferCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  ci.size = size;
  ci.usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
#ifdef _WIN32
  ci.pNext = &ext;
#endif
  if (vkCreateBuffer(ctx.vkDevice(), &ci, nullptr, &buffer_) != VK_SUCCESS)
    buffer_ = VK_NULL_HANDLE;
  if (!buffer_) return; // 创建失败：勿对空句柄查询内存需求（无效用法/UB）

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(ctx.vkDevice(), buffer_, &req);
  VkDeviceMemory mem = VK_NULL_HANDLE;
  if (ctx.createExportableMemory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 bufferExportable(ctx, ci.usage),
                                 mem) == Result::Ok) {
    memory_ = mem;
    vkBindBufferMemory(ctx.vkDevice(), buffer_, memory_, 0);
  }
}

ExportableBuffer::~ExportableBuffer() { destroy(); }

ExportableBuffer::ExportableBuffer(ExportableBuffer &&other) noexcept
    : ctx_(other.ctx_), buffer_(other.buffer_), memory_(other.memory_),
      size_(other.size_) {
  other.buffer_ = VK_NULL_HANDLE;
  other.memory_ = VK_NULL_HANDLE;
  other.exportedHandle_ = nullptr;
}

ExportableBuffer &
ExportableBuffer::operator=(ExportableBuffer &&other) noexcept {
  if (this != &other) {
    destroy();
    ctx_ = other.ctx_;
    buffer_ = other.buffer_;
    memory_ = other.memory_;
    size_ = other.size_;
    other.buffer_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.exportedHandle_ = nullptr;
  }
  return *this;
}

void *ExportableBuffer::exportHandle() {
  if (exportedHandle_) return exportedHandle_;
#ifdef _WIN32
  VkMemoryGetWin32HandleInfoKHR gi{};
  gi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
  gi.memory = memory_;
  gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  auto fn = ctx_->vkGetMemoryWin32Handle();
  if (!fn || fn(ctx_->vkDevice(), &gi, &exportedHandle_) != VK_SUCCESS)
    exportedHandle_ = nullptr;
#else
  // 非 Windows 不提供 GL 互操作：没有可导出的句柄。
#endif
  return exportedHandle_;
}

void ExportableBuffer::destroy() {
  // OPAQUE_WIN32 句柄归本对象所有，GL_EXT_memory_object(_win32) 不接管 opaque
  // 句柄；GL 侧仅 glImportMemoryWin32HandleEXT 引用同一块内存，不拥有句柄。
  if (exportedHandle_) {
#ifdef _WIN32
    CloseHandle(static_cast<HANDLE>(exportedHandle_));
#endif
    exportedHandle_ = nullptr;
  }
  if (memory_) vkFreeMemory(ctx_->vkDevice(), memory_, nullptr);
  if (buffer_) vkDestroyBuffer(ctx_->vkDevice(), buffer_, nullptr);
  memory_ = VK_NULL_HANDLE;
  buffer_ = VK_NULL_HANDLE;
}

ExportableImage::ExportableImage(Context &ctx, uint32_t width, uint32_t height,
                                 VkFormat format)
    : ctx_(&ctx), width_(width), height_(height) {
#ifdef _WIN32
  VkExternalMemoryImageCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#endif

  VkImageCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {width, height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
#ifdef _WIN32
  ci.pNext = &ext;
#endif
  if (vkCreateImage(ctx.vkDevice(), &ci, nullptr, &image_) != VK_SUCCESS)
    image_ = VK_NULL_HANDLE;
  if (!image_) return; // 创建失败：勿对空句柄查询内存需求（无效用法/UB）

  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(ctx.vkDevice(), image_, &req);
  size_ = req.size; // GL_EXT_memory_object_win32 导入需要真实字节数
  VkDeviceMemory mem = VK_NULL_HANDLE;
  if (ctx.createExportableMemory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 imageExportable(ctx, ci), mem) == Result::Ok) {
    memory_ = mem;
    vkBindImageMemory(ctx.vkDevice(), image_, memory_, 0);
  }

  if (image_) {
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx.vkDevice(), &vci, nullptr, &view_) != VK_SUCCESS)
      view_ = VK_NULL_HANDLE;
  }
}

ExportableImage::~ExportableImage() { destroy(); }

ExportableImage::ExportableImage(ExportableImage &&other) noexcept
    : ctx_(other.ctx_), image_(other.image_), view_(other.view_),
      memory_(other.memory_), width_(other.width_), height_(other.height_),
      size_(other.size_) {
  other.image_ = VK_NULL_HANDLE;
  other.view_ = VK_NULL_HANDLE;
  other.memory_ = VK_NULL_HANDLE;
  other.size_ = 0;
  other.exportedHandle_ = nullptr;
}

ExportableImage &ExportableImage::operator=(ExportableImage &&other) noexcept {
  if (this != &other) {
    destroy();
    ctx_ = other.ctx_;
    image_ = other.image_;
    view_ = other.view_;
    memory_ = other.memory_;
    width_ = other.width_;
    height_ = other.height_;
    size_ = other.size_;
    other.image_ = VK_NULL_HANDLE;
    other.view_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.exportedHandle_ = nullptr;
  }
  return *this;
}

void *ExportableImage::exportHandle() {
  if (exportedHandle_) return exportedHandle_;
#ifdef _WIN32
  VkMemoryGetWin32HandleInfoKHR gi{};
  gi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
  gi.memory = memory_;
  gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  auto fn = ctx_->vkGetMemoryWin32Handle();
  if (!fn || fn(ctx_->vkDevice(), &gi, &exportedHandle_) != VK_SUCCESS)
    exportedHandle_ = nullptr;
#else
  // 非 Windows 不提供 GL 互操作：没有可导出的句柄。
#endif
  return exportedHandle_;
}

void ExportableImage::destroy() {
  if (exportedHandle_) {
#ifdef _WIN32
    CloseHandle(static_cast<HANDLE>(exportedHandle_));
#endif
    exportedHandle_ = nullptr;
  }
  if (view_) vkDestroyImageView(ctx_->vkDevice(), view_, nullptr);
  if (memory_) vkFreeMemory(ctx_->vkDevice(), memory_, nullptr);
  if (image_) vkDestroyImage(ctx_->vkDevice(), image_, nullptr);
  view_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE;
  image_ = VK_NULL_HANDLE;
}

} // namespace vic
