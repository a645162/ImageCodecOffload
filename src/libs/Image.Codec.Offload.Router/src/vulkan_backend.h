// File      : vulkan_backend.h
// Brief     : Vulkan 后端适配器（把 Image.Codec.Vulkan 的 C API 接到 ICodecBackend）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#pragma once

#include <memory>

#include "backend.h"

namespace image_codec {
namespace offload {

// 本构建未包含该后端时返回 nullptr。
std::unique_ptr<ICodecBackend> makeVulkanBackend();

} // namespace offload
} // namespace image_codec
