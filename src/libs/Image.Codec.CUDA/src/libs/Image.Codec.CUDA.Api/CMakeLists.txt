# File: src/libs/Image.Codec.CUDA.Api/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# 对外稳定 C API（include/Image/Codec/CUDA/image_codec_cuda.h）实现库。
add_library(Image.Codec.CUDA.Api STATIC
  src/image_codec_cuda.cpp
  "${PROJECT_SOURCE_DIR}/include/Image/Codec/CUDA/image_codec_cuda.h"
  "${PROJECT_SOURCE_DIR}/include/Image/Codec/CUDA/image_codec_cuda.hpp")

target_include_directories(Image.Codec.CUDA.Api PUBLIC
  "${PROJECT_SOURCE_DIR}/include")

target_link_libraries(Image.Codec.CUDA.Api PUBLIC
  Image.Codec.CUDA::Core
  Image.Codec.CUDA::Common
  Image.Codec.CUDA::Tiff
  Image.Codec.CUDA::NvJpeg
  Image.Codec.CUDA::Png
  Image.Codec.CUDA::Metrics)

# 共享库场景下 IMAGE_CODEC_CUDA_API 走 dllexport 分支；静态库下为空。
target_compile_definitions(Image.Codec.CUDA.Api PRIVATE
  IMAGE_CODEC_CUDA_BUILDING_LIB)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.CUDA::Api)
add_library(Image.Codec.CUDA::Api ALIAS Image.Codec.CUDA.Api)
