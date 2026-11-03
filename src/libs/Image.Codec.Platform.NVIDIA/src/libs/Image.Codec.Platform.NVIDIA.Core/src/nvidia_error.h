/*
 * File: nvidia_error.h
 * Brief: CUDA Runtime error to readable text.
 * Author: Haomin Kong
 * Email: konghm@beim.com.cn / a645162@163.com
 * Created: 2026-11-03
 */

// nvidia_error.h - CUDA Runtime 错误到可读文本的统一转换（带调用点上下文）。
//
// nvJPEG / nvJPEG2000 / nvTIFF 各自的 status 文本留在各自的 codec 里：
// 它们是三套互不相同的枚举，混进这里只会变成一张谁也不敢删的大 switch。
#pragma once

#include <cuda_runtime_api.h>

#include <string>

namespace image_codec {
namespace nvidia {

// cudaError_t -> "cudaErrorXxx: 描述"。
std::string cuda_error_text(cudaError_t e);

// 失败时写 err = "<what>: <cuda_error_text>"，返回 false；成功返回 true。
inline bool check_cuda(cudaError_t e, const char *what, std::string *err) {
  if (e == cudaSuccess) {
    return true;
  }
  if (err != nullptr) {
    *err = std::string(what != nullptr ? what : "cuda call") + ": " +
           cuda_error_text(e);
  }
  return false;
}

} // namespace nvidia
} // namespace image_codec
