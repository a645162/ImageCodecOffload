# File: src/libs/ImageCodecOffload.Core/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

add_library(ImageCodecOffload.Core STATIC
  src/image_codec_types.h
  src/image_codec_types.cpp
  src/metrics.h
  src/metrics.cpp
  src/backend.h)

target_include_directories(ImageCodecOffload.Core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Core 是纯 C++：不依赖 Qt、不依赖任何 GPU SDK、不链接 libtiff/libpng。
# 这条约束保证「后端接口 + 指标 CPU 参考」可以单独被 SDK 与测试使用。
add_library(ImageCodecOffload::Core ALIAS ImageCodecOffload.Core)
