/*
 * File: compute.cpp
 * Brief: Compute.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#include "compute.h"
#include "vic_spv_data.h"
#include <array>

namespace vic {

Compute::~Compute() { destroy(); }

void Compute::destroy() {
  auto d = ctx_.vkDevice();
  if (bufferPipeline_) vkDestroyPipeline(d, bufferPipeline_, nullptr);
  if (imagePipeline_) vkDestroyPipeline(d, imagePipeline_, nullptr);
  if (pipelineLayout_) vkDestroyPipelineLayout(d, pipelineLayout_, nullptr);
  if (dsLayout_) vkDestroyDescriptorSetLayout(d, dsLayout_, nullptr);
  if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
  if (cmdPool_) vkDestroyCommandPool(d, cmdPool_, nullptr);
  bufferPipeline_ = imagePipeline_ = VK_NULL_HANDLE;
  pipelineLayout_ = VK_NULL_HANDLE;
  dsLayout_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  cmdPool_ = VK_NULL_HANDLE;
}

Result Compute::init() {
  auto d = ctx_.vkDevice();

  VkDescriptorSetLayoutBinding bindings[2]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[1] = bindings[0];
  bindings[1].binding = 1;  // 修复：bindings[1]=bindings[0] 会继承 binding=0，需显式置 1
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

  VkDescriptorSetLayoutCreateInfo dsl{};
  dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dsl.bindingCount = 2;
  dsl.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(d, &dsl, nullptr, &dsLayout_) != VK_SUCCESS)
    return Result::InitFailed;

  VkPushConstantRange pc{};
  pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pc.offset = 0;
  pc.size = 12;  // width, height, mode (3 × uint32)

  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &dsLayout_;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &pc;
  if (vkCreatePipelineLayout(d, &pl, nullptr, &pipelineLayout_) != VK_SUCCESS)
    return Result::InitFailed;

  VkShaderModuleCreateInfo smc{};
  smc.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  smc.codeSize = vic_gradient_buffer_size * sizeof(uint32_t);
  smc.pCode = vic_gradient_buffer;
  VkShaderModule bufMod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(d, &smc, nullptr, &bufMod) != VK_SUCCESS)
    return Result::InitFailed;
  smc.codeSize = vic_gradient_texture_size * sizeof(uint32_t);
  smc.pCode = vic_gradient_texture;
  VkShaderModule imgMod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(d, &smc, nullptr, &imgMod) != VK_SUCCESS) {
    vkDestroyShaderModule(d, bufMod, nullptr);
    return Result::InitFailed;
  }

  if (createPipeline(bufMod, bufferPipeline_) != Result::Ok ||
      createPipeline(imgMod, imagePipeline_) != Result::Ok) {
    vkDestroyShaderModule(d, bufMod, nullptr);
    vkDestroyShaderModule(d, imgMod, nullptr);
    return Result::InitFailed;
  }
  vkDestroyShaderModule(d, bufMod, nullptr);
  vkDestroyShaderModule(d, imgMod, nullptr);

  VkDescriptorPoolSize sizes[2]{};
  sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  sizes[0].descriptorCount = 2;  // bufferSet_ + imageSet_ 各含 binding0(STORAGE_BUFFER)
  sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  sizes[1].descriptorCount = 2;  // 各含 binding1(STORAGE_IMAGE)
  VkDescriptorPoolCreateInfo dpc{};
  dpc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpc.maxSets = 2;
  dpc.poolSizeCount = 2;
  dpc.pPoolSizes = sizes;
  if (vkCreateDescriptorPool(d, &dpc, nullptr, &pool_) != VK_SUCCESS)
    return Result::InitFailed;

  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = pool_;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &dsLayout_;
  if (vkAllocateDescriptorSets(d, &dsai, &bufferSet_) != VK_SUCCESS)
    return Result::InitFailed;
  if (vkAllocateDescriptorSets(d, &dsai, &imageSet_) != VK_SUCCESS)
    return Result::InitFailed;

  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.queueFamilyIndex = ctx_.queueFamilyIndex();
  if (vkCreateCommandPool(d, &cpi, nullptr, &cmdPool_) != VK_SUCCESS)
    return Result::InitFailed;
  VkCommandBufferAllocateInfo cba{};
  cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cba.commandPool = cmdPool_;
  cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cba.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(d, &cba, &cmd_) != VK_SUCCESS)
    return Result::InitFailed;

  ready_ = true;
  return Result::Ok;
}

Result Compute::createPipeline(VkShaderModule module, VkPipeline& out) {
  VkComputePipelineCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  ci.stage.module = module;
  ci.stage.pName = "main";
  ci.layout = pipelineLayout_;
  if (vkCreateComputePipelines(ctx_.vkDevice(), VK_NULL_HANDLE, 1, &ci, nullptr, &out) != VK_SUCCESS)
    return Result::InitFailed;
  return Result::Ok;
}

void Compute::submit() {
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  vkQueueSubmit(ctx_.vkQueue(), 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(ctx_.vkQueue());
}

void Compute::writeBuffer(ExportableBuffer& buf, uint32_t w, uint32_t h) {
  VkDescriptorBufferInfo bi{};
  bi.buffer = buf.vkBuffer();
  bi.range = VK_WHOLE_SIZE;
  VkWriteDescriptorSet wd{};
  wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  wd.dstSet = bufferSet_;
  wd.dstBinding = 0;
  wd.descriptorCount = 1;
  wd.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  wd.pBufferInfo = &bi;
  vkUpdateDescriptorSets(ctx_.vkDevice(), 1, &wd, 0, nullptr);

  VkCommandBufferBeginInfo bb{};
  bb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(cmd_, &bb);
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, bufferPipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &bufferSet_, 0, nullptr);
  std::array<uint32_t, 3> pcv = {w, h, 0};
  vkCmdPushConstants(cmd_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, pcv.data());
  vkCmdDispatch(cmd_, (w * h + 63) / 64, 1, 1);
  vkEndCommandBuffer(cmd_);
  submit();
}

void Compute::writeImage(ExportableImage& img, uint32_t w, uint32_t h) {
  VkDescriptorImageInfo ii{};
  ii.imageView = img.vkView();
  ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet wd{};
  wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  wd.dstSet = imageSet_;
  wd.dstBinding = 1;
  wd.descriptorCount = 1;
  wd.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  wd.pImageInfo = &ii;
  vkUpdateDescriptorSets(ctx_.vkDevice(), 1, &wd, 0, nullptr);

  VkImageMemoryBarrier pre{};
  pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  pre.image = img.vkImage();
  pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  pre.srcAccessMask = 0;
  pre.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

  VkCommandBufferBeginInfo bb{};
  bb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(cmd_, &bb);
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &pre);
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, imagePipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &imageSet_, 0, nullptr);
  std::array<uint32_t, 3> pcv = {w, h, 1};
  vkCmdPushConstants(cmd_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, pcv.data());
  vkCmdDispatch(cmd_, (w + 7) / 8, (h + 7) / 8, 1);
  vkEndCommandBuffer(cmd_);
  submit();
}

}  // namespace vic
