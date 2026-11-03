# File: src/libs/Image.Codec.Platform.NVIDIA.Tiff/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

# nvTIFF（TIFF 容器 + LZW / Deflate / JPEG / PackBits）封装。nvTIFF 是独立安装
# 的包，由顶层 find_path / find_library 成对探测后建出 NVIDIA::nvtiff。
add_library(Image.Codec.Platform.NVIDIA.Tiff STATIC
  src/nvtiff_codec.h
  src/nvtiff_codec.cpp)

target_include_directories(Image.Codec.Platform.NVIDIA.Tiff PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.Platform.NVIDIA.Tiff PUBLIC
  Image.Codec.Platform.NVIDIA::Core
  NVIDIA::nvtiff)

target_compile_definitions(Image.Codec.Platform.NVIDIA.Tiff PRIVATE
  OIC_NVIDIA_HAS_NVTIFF=1)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.Platform.NVIDIA::Tiff)
add_library(Image.Codec.Platform.NVIDIA::Tiff ALIAS Image.Codec.Platform.NVIDIA.Tiff)
