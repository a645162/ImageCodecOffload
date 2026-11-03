# File: src/libs/Image.Codec.CUDA.Tiff/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# TIFF 模块：容器在 Common，LZW 编码在 CUDA kernel，解码走 CPU 参考实现。
add_library(Image.Codec.CUDA.Tiff STATIC
  src/tiff_codec.h
  src/tiff_codec.cpp
  src/lzw_encode_cuda.h
  src/lzw_encode_cuda.cpp
  src/lzw_encode_launch.h
  src/lzw_encode_launch.cu
  src/kernels/lzw_encode.cu)

target_include_directories(Image.Codec.CUDA.Tiff PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.CUDA.Tiff PUBLIC
  Image.Codec.CUDA::Core
  Image.Codec.CUDA::Common
  CUDA::cudart)

# kernel 只用 CUDA Runtime API，不需要 separable compilation。
set_target_properties(Image.Codec.CUDA.Tiff PROPERTIES
  CUDA_SEPARABLE_COMPILATION OFF)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.CUDA::Tiff)
add_library(Image.Codec.CUDA::Tiff ALIAS Image.Codec.CUDA.Tiff)
