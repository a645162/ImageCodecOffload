// File      : vk_pipeline.h
// Brief     : Vulkan compute pipeline 的最小封装（storage buffer 绑定 + push constant）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 编解码/指标用到的着色器形状都一样：若干只读存储缓冲 + 若干可写存储缓冲 +
// 一小段 push constant。因此这里不做通用反射，只支持这一种绑定模型——
// 每多一样东西，就要多一套「描述符写错了却照样能跑」的可能性。
//
// 派发是同步的（提交后 vkQueueWaitIdle）：一次 dispatch 之后立刻要回读结果，
// 异步只能让调用方去管 fence，换来的是没人用的并发。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 先包含 vk_codec_device.h：它经由 vic.hpp 定义 VK_USE_PLATFORM_WIN32_KHR，
// 必须早于 <vulkan/vulkan.h>，否则 vulkan_win32.h 不会被引入。
#include "vk_codec_device.h"

#include <vulkan/vulkan.h>

namespace image_codec {
namespace vulkan {

struct BufferBinding {
  uint32_t binding = 0;
  // true = 着色器里写这块缓冲（readonly 与 writeonly/读写会影响验证层与驱动的
  // 优化，所以必须由调用方如实声明，不能一律当可读写）。
  bool writable = false;
};

class Pipeline {
public:
  Pipeline() = default;
  ~Pipeline();
  Pipeline(const Pipeline &) = delete;
  Pipeline &operator=(const Pipeline &) = delete;

  bool init(Device &device, const uint32_t *spirv, size_t word_count,
            const std::vector<BufferBinding> &bindings,
            size_t push_constant_bytes, std::string *error);

  bool ready() const { return pipeline_ != VK_NULL_HANDLE; }

  // buffers 必须与 init() 的 bindings 数量、顺序一致。
  bool dispatch(const std::vector<VkBuffer> &buffers, const void *push,
                size_t push_bytes, uint32_t groups, std::string *error);

private:
  void destroy();

  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout ds_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  std::vector<BufferBinding> bindings_;
};

} // namespace vulkan
} // namespace image_codec
