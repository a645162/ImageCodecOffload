# File: src/libs/Image.Codec.Platform.NVIDIA.Core/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

# 基础设施：设备枚举 / 显式 stream / 显存记账 / 设备缓冲 / CUDA 错误文本。
# 只依赖 CUDA Runtime（cudart），不需要 nvcc 编译本目标。
add_library(Image.Codec.Platform.NVIDIA.Core STATIC
  src/image_buffer.h
  src/nvidia_error.h
  src/nvidia_error.cpp
  src/nvidia_runtime.h
  src/nvidia_runtime.cpp)

target_include_directories(Image.Codec.Platform.NVIDIA.Core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.Platform.NVIDIA.Core PUBLIC CUDA::cudart)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.Platform.NVIDIA::Core)
add_library(Image.Codec.Platform.NVIDIA::Core ALIAS Image.Codec.Platform.NVIDIA.Core)
