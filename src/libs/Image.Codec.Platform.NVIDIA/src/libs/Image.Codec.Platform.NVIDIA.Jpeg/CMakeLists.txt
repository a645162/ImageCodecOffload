# File: src/libs/Image.Codec.Platform.NVIDIA.Jpeg/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

# nvJPEG（baseline JPEG）封装。依赖 CUDA Toolkit 自带的 CUDA::nvjpeg 与 cudart，
# 不需要 nvcc 编译本目标。
add_library(Image.Codec.Platform.NVIDIA.Jpeg STATIC
  src/nvjpeg_codec.h
  src/nvjpeg_codec.cpp)

target_include_directories(Image.Codec.Platform.NVIDIA.Jpeg PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.Platform.NVIDIA.Jpeg PUBLIC
  Image.Codec.Platform.NVIDIA::Core
  CUDA::nvjpeg)

target_compile_definitions(Image.Codec.Platform.NVIDIA.Jpeg PRIVATE
  OIC_NVIDIA_HAS_NVJPEG=1)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.Platform.NVIDIA::Jpeg)
add_library(Image.Codec.Platform.NVIDIA::Jpeg ALIAS Image.Codec.Platform.NVIDIA.Jpeg)
