# File: src/libs/Image.Codec.Vulkan.Api/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

add_library(Image.Codec.Vulkan.Api STATIC src/gl_image_pipeline.cpp)
target_include_directories(Image.Codec.Vulkan.Api PUBLIC
  "${VIC_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.Vulkan.Api PUBLIC Image.Codec.Vulkan::Core Image.Codec.Vulkan::GlInterop)
add_library(Image.Codec.Vulkan::Api ALIAS Image.Codec.Vulkan.Api)
