# File: src/libs/Image.Codec.CUDA.Core/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# CUDA 基础设施：设备/流/缓冲 RAII、错误文本、显存查询与预算记账、宿主侧计时。
add_library(Image.Codec.CUDA.Core STATIC
  src/cuda_error.h
  src/cuda_error.cpp
  src/cuda_buffer.h
  src/cuda_memory.h
  src/cuda_memory.cpp
  src/cuda_device.h
  src/cuda_device.cpp
  src/platform.h
  src/platform.cpp)

target_include_directories(Image.Codec.CUDA.Core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.CUDA.Core PUBLIC CUDA::cudart)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.CUDA::Core)
add_library(Image.Codec.CUDA::Core ALIAS Image.Codec.CUDA.Core)
