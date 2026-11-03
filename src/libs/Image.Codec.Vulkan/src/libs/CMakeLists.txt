# File: src/libs/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

add_subdirectory(Image.Codec.Vulkan.Core)
add_subdirectory(Image.Codec.Vulkan.Common)
add_subdirectory(Image.Codec.Vulkan.Metrics)
add_subdirectory(Image.Codec.Vulkan.Png)
add_subdirectory(Image.Codec.Vulkan.Tiff)
add_subdirectory(Image.Codec.Vulkan.Jpeg)
add_subdirectory(Image.Codec.Vulkan.CodecApi)
if(WIN32)
  add_subdirectory(Image.Codec.Vulkan.GlInterop)
  add_subdirectory(Image.Codec.Vulkan.Api)
endif()
