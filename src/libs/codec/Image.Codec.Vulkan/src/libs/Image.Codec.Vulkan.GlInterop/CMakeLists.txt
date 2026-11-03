# File: src/libs/Image.Codec.Vulkan.GlInterop/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

add_library(Image.Codec.Vulkan.GlInterop STATIC src/gl_interop.cpp)
target_include_directories(Image.Codec.Vulkan.GlInterop PUBLIC
  "${VIC_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.Vulkan.GlInterop PUBLIC
  Image.Codec.Vulkan::Core
  opengl32)
add_library(Image.Codec.Vulkan::GlInterop ALIAS Image.Codec.Vulkan.GlInterop)
