/*
 * File: gl_image_pipeline.cpp
 * Brief: Gl image pipeline.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#include "gl_image_pipeline.h"
#include "gl_interop.h"
#include "context.h"
#include "exportable.h"
#include "compute.h"

namespace vic {

struct GlImagePipeline::Impl {
  std::unique_ptr<Compute> compute;
  std::unique_ptr<ExportableBuffer> buffer;
  std::unique_ptr<ExportableImage> image;
};

GlImagePipeline::GlImagePipeline() = default;

GlImagePipeline::~GlImagePipeline() {
  if (glBuffer_) GlInterop::releaseObject(glBuffer_, false);
  if (glTexture_) GlInterop::releaseObject(glTexture_, true);
}

Result GlImagePipeline::init(Context& ctx, uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;
  impl_ = std::make_unique<Impl>();
  impl_->compute = std::make_unique<Compute>(ctx);
  auto r = impl_->compute->init();
  if (r != Result::Ok) return r;
  VkDeviceSize bufSize = static_cast<VkDeviceSize>(width) * height * 4;
  bufSize = (bufSize + 3) & ~static_cast<VkDeviceSize>(3);
  impl_->buffer = std::make_unique<ExportableBuffer>(ctx, bufSize);
  if (!impl_->buffer->vkBuffer()) return Result::Error;
  impl_->image = std::make_unique<ExportableImage>(ctx, width, height, VK_FORMAT_R8G8B8A8_UNORM);
  if (!impl_->image->vkImage()) return Result::Error;
  return Result::Ok;
}

Result GlImagePipeline::runBuffer() {
  if (glBuffer_) return Result::Ok;
  if (!impl_ || !impl_->buffer) return Result::Error;
  impl_->compute->writeBuffer(*impl_->buffer, width_, height_);
  void* h = impl_->buffer->exportHandle();
  if (!h) return Result::Error;
  glBuffer_ = GlInterop::importBuffer(h, static_cast<uint64_t>(impl_->buffer->size()));
  return glBuffer_ ? Result::Ok : Result::Error;
}

Result GlImagePipeline::runTexture() {
  if (glTexture_) return Result::Ok;
  if (!impl_ || !impl_->image) return Result::Error;
  impl_->compute->writeImage(*impl_->image, width_, height_);
  void* h = impl_->image->exportHandle();
  if (!h) return Result::Error;
  glTexture_ = GlInterop::importTexture(h, width_, height_, impl_->image->memorySize());
  return glTexture_ ? Result::Ok : Result::Error;
}

uint32_t GlImagePipeline::glBuffer() const { return glBuffer_; }
uint32_t GlImagePipeline::glTexture() const { return glTexture_; }

}  // namespace vic
