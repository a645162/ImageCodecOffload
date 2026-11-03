# File: src/libs/Image.Codec.CUDA.Png/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# PNG 模块：行滤波在 CUDA kernel，deflate/inflate 在 zlib。
add_library(Image.Codec.CUDA.Png STATIC
  src/png_codec.h
  src/png_codec.cpp
  src/png_filter_cuda.h
  src/png_filter_cuda.cpp
  src/png_filter_launch.h
  src/png_filter_launch.cu
  src/kernels/png_filter.cu)

target_include_directories(Image.Codec.CUDA.Png PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.CUDA.Png PUBLIC
  Image.Codec.CUDA::Core
  Image.Codec.CUDA::Common
  CUDA::cudart
  ZLIB::ZLIB)

set_target_properties(Image.Codec.CUDA.Png PROPERTIES
  CUDA_SEPARABLE_COMPILATION OFF)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.CUDA::Png)
add_library(Image.Codec.CUDA::Png ALIAS Image.Codec.CUDA.Png)
