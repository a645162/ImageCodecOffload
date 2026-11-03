/*
 * File: gl_interop.h
 * Brief: Gl interop.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#pragma once
#include <Image/Codec/Vulkan/vic.hpp>

namespace vic {

class GlInterop {
public:
  static Result init();
  static bool available();
  static uint32_t importBuffer(void* handle, uint64_t size);
  static uint32_t importTexture(void* handle, uint32_t w, uint32_t h, uint64_t size);
  static void releaseObject(uint32_t obj, bool isTexture);
};

}  // namespace vic
