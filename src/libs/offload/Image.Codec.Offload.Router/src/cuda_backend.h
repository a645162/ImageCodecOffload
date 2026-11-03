/*
 * File: cuda_backend.h
 * Brief: Cuda backend.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// cuda_backend.h - CUDA 后端适配器（把 Image.Codec.CUDA 的 C API 接到 ICodecBackend）。
#pragma once

#include <memory>

#include "backend.h"

namespace image_codec {
namespace offload {

// 本构建未包含该后端时返回 nullptr。
std::unique_ptr<ICodecBackend> makeCudaBackend();

}  // namespace offload
}  // namespace image_codec
