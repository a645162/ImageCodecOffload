/*
 * File: nvidia_error.cpp
 * Brief: CUDA Runtime error to readable text.
 * Author: Haomin Kong
 * Email: konghm@beim.com.cn / a645162@163.com
 * Created: 2026-11-03
 */

#include "nvidia_error.h"

namespace image_codec {
namespace nvidia {

std::string cuda_error_text(cudaError_t e) {
  const char *name = cudaGetErrorName(e);
  const char *text = cudaGetErrorString(e);
  std::string result = name != nullptr ? name : "cudaErrorUnknown";
  if (text != nullptr) {
    result += ": ";
    result += text;
  }
  return result;
}

} // namespace nvidia
} // namespace image_codec
