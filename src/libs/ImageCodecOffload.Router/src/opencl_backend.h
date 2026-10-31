/*
 * File: opencl_backend.h
 * Brief: Opencl backend.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// opencl_backend.h - OpenCL 后端适配器（把 OpenCLImageCodec 的 C API 接到 ICodecBackend）。
//
// 只在 IMAGECODEC_OFFLOAD_WITH_OPENCL 打开时编译。适配层只做三件事：
//   1. offload::Image <-> image_codec_image 的转换（C ABI 没有所有权概念，必须复制）；
//   2. 状态码映射（两边的数值本来就一一对应，映射只是显式写出来）；
//   3. 失败原因补上后端名，否则上层日志里只剩一句「encode failed」。
#pragma once

#include <memory>

#include "backend.h"

namespace image_codec {
namespace offload {

// 本构建未包含该后端时返回 nullptr。
std::unique_ptr<ICodecBackend> makeOpenClBackend();

}  // namespace offload
}  // namespace image_codec
