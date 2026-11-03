# File: src/libs/Image.Codec.CUDA.NvJpeg/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# Image.Codec.CUDA.NvJpeg —— NVIDIA 固定功能硬件编解码（NVJPG）封装。
#
# 与 Image.Codec.Platform.Intel 的分工：Intel 的 oneVPL/QuickSync 是另一家厂商的硬件路径，
# 归 Image.Codec.Platform.Intel 的 Image.Codec.Platform.Intel.Jpeg.oneVPL；NVIDIA 的 nvJPEG 由本模块
# 承载，只依赖 CUDA Toolkit 的 nvjpeg 组件（不需要 nvcc 编译本模块）。
add_library(Image.Codec.CUDA.NvJpeg STATIC
  src/jpeg_codec.h
  src/jpeg_codec.cpp)

target_include_directories(Image.Codec.CUDA.NvJpeg PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.CUDA.NvJpeg PUBLIC
  Image.Codec.CUDA::Core
  Image.Codec.CUDA::Common
  CUDA::cudart)

if(TARGET CUDA::nvjpeg AND ICUDA_ENABLE_NVJPEG)
  target_compile_definitions(Image.Codec.CUDA.NvJpeg PRIVATE ICUDA_HAS_NVJPEG=1)
  target_link_libraries(Image.Codec.CUDA.NvJpeg PUBLIC CUDA::nvjpeg)
endif()

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.CUDA::NvJpeg)
add_library(Image.Codec.CUDA::NvJpeg ALIAS Image.Codec.CUDA.NvJpeg)
