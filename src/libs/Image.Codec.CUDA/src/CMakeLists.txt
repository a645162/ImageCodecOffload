# File: src/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# src/ 下为多个独立静态库（参照 Image.Codec.OpenCL 组织），用户可按需链接：
#   target_link_libraries(app PRIVATE Image.Codec.CUDA::Tiff)   # 仅 TIFF（PUBLIC 传播 Core/Common）
add_subdirectory(libs)

# 全量聚合便捷入口（可选）：一次链接全部库（含 C API）。
add_library(Image.Codec.CUDA INTERFACE)
target_include_directories(Image.Codec.CUDA INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>")
target_link_libraries(Image.Codec.CUDA INTERFACE
  Image.Codec.CUDA::Core
  Image.Codec.CUDA::Tiff
  Image.Codec.CUDA::NvJpeg
  Image.Codec.CUDA::Png
  Image.Codec.CUDA::Api
  Image.Codec.CUDA::Metrics
  Image.Codec.CUDA::Tune)
add_library(Image.Codec.CUDA::All ALIAS Image.Codec.CUDA)
