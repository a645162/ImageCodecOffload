/*
 * File: compute.h
 * Brief: Compute.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#pragma once
#include <Image/Codec/Vulkan/vic.hpp>
#include "context.h"
#include "exportable.h"
#include <vulkan/vulkan.h>

namespace vic {

class Compute {
public:
  explicit Compute(Context& ctx) : ctx_(ctx) {}
  ~Compute();
  Compute(const Compute&) = delete;
  Compute& operator=(const Compute&) = delete;

  Result init();
  void writeBuffer(ExportableBuffer& buf, uint32_t w, uint32_t h);
  void writeImage(ExportableImage& img, uint32_t w, uint32_t h);

private:
  void destroy();
  Result createPipeline(VkShaderModule module, VkPipeline& out);
  void submit();

  Context& ctx_;
  VkDescriptorSetLayout dsLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline bufferPipeline_ = VK_NULL_HANDLE;
  VkPipeline imagePipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet bufferSet_ = VK_NULL_HANDLE;
  VkDescriptorSet imageSet_ = VK_NULL_HANDLE;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  bool ready_ = false;
};

}  // namespace vic
