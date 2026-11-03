// File      : vk_pipeline.cpp
// Brief     : Vulkan compute pipeline 实现
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#include "vk_pipeline.h"

#include <cstring>

namespace image_codec {
namespace vulkan {

Pipeline::~Pipeline() { destroy(); }

bool Pipeline::init(Device &device, const uint32_t *spirv, size_t word_count,
                    const std::vector<BufferBinding> &bindings,
                    size_t push_constant_bytes, std::string *error) {
  destroy();
  device_ = device.context().vkDevice();
  queue_ = device.context().vkQueue();
  bindings_ = bindings;

  if (spirv == nullptr || word_count == 0) {
    if (error != nullptr) *error = "vulkan: empty SPIR-V module";
    return false;
  }
  if (bindings.empty()) {
    if (error != nullptr)
      *error = "vulkan: pipeline needs at least one binding";
    return false;
  }
  if (push_constant_bytes > 128) {
    if (error != nullptr)
      *error = "vulkan: push constant block exceeds 128 bytes";
    return false;
  }

  std::vector<VkDescriptorSetLayoutBinding> layout_bindings(bindings.size());
  for (size_t i = 0; i < bindings.size(); ++i) {
    layout_bindings[i].binding = bindings[i].binding;
    layout_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    layout_bindings[i].descriptorCount = 1;
    layout_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo dsl{};
  dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dsl.bindingCount = static_cast<uint32_t>(layout_bindings.size());
  dsl.pBindings = layout_bindings.data();
  if (vkCreateDescriptorSetLayout(device_, &dsl, nullptr, &ds_layout_) !=
      VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkCreateDescriptorSetLayout failed";
    return false;
  }

  VkPushConstantRange pc{};
  pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pc.offset = 0;
  pc.size = static_cast<uint32_t>(push_constant_bytes);

  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &ds_layout_;
  pl.pushConstantRangeCount = push_constant_bytes > 0 ? 1 : 0;
  pl.pPushConstantRanges = push_constant_bytes > 0 ? &pc : nullptr;
  if (vkCreatePipelineLayout(device_, &pl, nullptr, &layout_) != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkCreatePipelineLayout failed";
    destroy();
    return false;
  }

  VkShaderModuleCreateInfo smc{};
  smc.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  smc.codeSize = word_count * sizeof(uint32_t);
  smc.pCode = spirv;
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &smc, nullptr, &module) != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkCreateShaderModule failed";
    destroy();
    return false;
  }

  VkComputePipelineCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  ci.stage.module = module;
  ci.stage.pName = "main";
  ci.layout = layout_;
  const VkResult create = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1,
                                                   &ci, nullptr, &pipeline_);
  vkDestroyShaderModule(device_, module, nullptr);
  if (create != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkCreateComputePipelines failed";
    destroy();
    return false;
  }

  VkDescriptorPoolSize size{};
  size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  size.descriptorCount = static_cast<uint32_t>(bindings.size());
  VkDescriptorPoolCreateInfo dpc{};
  dpc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpc.maxSets = 1;
  dpc.poolSizeCount = 1;
  dpc.pPoolSizes = &size;
  if (vkCreateDescriptorPool(device_, &dpc, nullptr, &pool_) != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkCreateDescriptorPool failed";
    destroy();
    return false;
  }

  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = pool_;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &ds_layout_;
  if (vkAllocateDescriptorSets(device_, &dsai, &set_) != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkAllocateDescriptorSets failed";
    destroy();
    return false;
  }

  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.queueFamilyIndex = device.context().queueFamilyIndex();
  if (vkCreateCommandPool(device_, &cpi, nullptr, &cmd_pool_) != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkCreateCommandPool failed";
    destroy();
    return false;
  }

  VkCommandBufferAllocateInfo cba{};
  cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cba.commandPool = cmd_pool_;
  cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cba.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device_, &cba, &cmd_) != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkAllocateCommandBuffers failed";
    destroy();
    return false;
  }

  return true;
}

bool Pipeline::dispatch(const std::vector<VkBuffer> &buffers, const void *push,
                        size_t push_bytes, uint32_t groups,
                        std::string *error) {
  if (pipeline_ == VK_NULL_HANDLE) {
    if (error != nullptr) *error = "vulkan: pipeline not initialised";
    return false;
  }
  if (buffers.size() != bindings_.size()) {
    if (error != nullptr)
      *error = "vulkan: buffer count does not match bindings";
    return false;
  }
  if (groups == 0) groups = 1;

  std::vector<VkDescriptorBufferInfo> infos(buffers.size());
  std::vector<VkWriteDescriptorSet> writes(buffers.size());
  for (size_t i = 0; i < buffers.size(); ++i) {
    infos[i].buffer = buffers[i];
    infos[i].offset = 0;
    infos[i].range = VK_WHOLE_SIZE;

    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set_;
    writes[i].dstBinding = bindings_[i].binding;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);

  vkResetCommandBuffer(cmd_, 0);

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &begin) != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkBeginCommandBuffer failed";
    return false;
  }
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1,
                          &set_, 0, nullptr);
  if (push != nullptr && push_bytes > 0) {
    vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       static_cast<uint32_t>(push_bytes), push);
  }

  // 着色器写 storage buffer，回读之前必须让写入对宿主可见。
  // 全内存屏障比逐缓冲 vkCmdPipelineBarrier 省事，代价在这种一次性派发里
  // 可以忽略（每次 dispatch 本来就要 vkQueueWaitIdle）。
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

  vkCmdDispatch(cmd_, groups, 1, 1);
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                       0, nullptr);
  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkEndCommandBuffer failed";
    return false;
  }

  if (queue_ == VK_NULL_HANDLE) {
    if (error != nullptr) *error = "vulkan: no queue bound to pipeline";
    return false;
  }

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd_;
  if (vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkQueueSubmit failed";
    return false;
  }
  if (vkQueueWaitIdle(queue_) != VK_SUCCESS) {
    if (error != nullptr) *error = "vulkan: vkQueueWaitIdle failed";
    return false;
  }
  return true;
}

void Pipeline::destroy() {
  if (device_ == VK_NULL_HANDLE) return;
  if (cmd_pool_ != VK_NULL_HANDLE)
    vkDestroyCommandPool(device_, cmd_pool_, nullptr);
  if (pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, pool_, nullptr);
  if (pipeline_ != VK_NULL_HANDLE)
    vkDestroyPipeline(device_, pipeline_, nullptr);
  if (layout_ != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(device_, layout_, nullptr);
  if (ds_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device_, ds_layout_, nullptr);
  }
  cmd_ = VK_NULL_HANDLE;
  cmd_pool_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  set_ = VK_NULL_HANDLE;
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  ds_layout_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  queue_ = VK_NULL_HANDLE;
}

} // namespace vulkan
} // namespace image_codec
