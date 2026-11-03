/*
 * File: gl_image_pipeline.h
 * Brief: Gl image pipeline.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#pragma once
#include <Image/Codec/Vulkan/vic.hpp>
#include <memory>

namespace vic {

class GlImagePipeline {
public:
  GlImagePipeline();  // 定义在 .cpp（pimpl：unique_ptr<Impl> 需完整类型）
  ~GlImagePipeline();
  GlImagePipeline(const GlImagePipeline&) = delete;
  GlImagePipeline& operator=(const GlImagePipeline&) = delete;

  Result init(Context& ctx, uint32_t width, uint32_t height);
  Result runBuffer();
  Result runTexture();

  uint32_t glBuffer() const;
  uint32_t glTexture() const;
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t glBuffer_ = 0;
  uint32_t glTexture_ = 0;
};

}  // namespace vic
