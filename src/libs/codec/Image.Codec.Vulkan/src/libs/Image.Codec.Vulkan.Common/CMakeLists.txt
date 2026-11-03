# File: src/libs/Image.Codec.Vulkan.Common/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# Image.Codec.Vulkan.Common —— 编解码共用层（平台无关）。
#
# 两部分：
#   * 设备/缓冲/管线（vk_codec_device / vk_buffer / vk_pipeline）：GPU 侧公共设施；
#   * 容器与像素的宿主侧实现（png_container / tiff_container / lzw_codec）：
#     与 GPU 无关，但三个格式模块都要用，且语义必须与 CUDA / OpenCL 后端一致。
add_library(Image.Codec.Vulkan.Common STATIC
  src/image_buffer.h
  src/vk_codec_device.h
  src/vk_codec_device.cpp
  src/vk_buffer.h
  src/vk_buffer.cpp
  src/vk_pipeline.h
  src/vk_pipeline.cpp
  src/png_container.h
  src/png_container.cpp
  src/tiff_container.h
  src/tiff_container.cpp
  src/lzw_codec.h
  src/lzw_codec.cpp)

target_include_directories(Image.Codec.Vulkan.Common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

# zlib：PNG 的 deflate/inflate（容器层）。deflate 留在 CPU 是刻意的：
# 这些尺寸下 GPU deflate 没有收益，还会让码流兼容性变成不可验证的问题。
find_package(ZLIB REQUIRED)
target_link_libraries(Image.Codec.Vulkan.Common PUBLIC
  Image.Codec.Vulkan::Core
  Vulkan::Vulkan
  ZLIB::ZLIB)

add_library(Image.Codec.Vulkan::Common ALIAS Image.Codec.Vulkan.Common)
