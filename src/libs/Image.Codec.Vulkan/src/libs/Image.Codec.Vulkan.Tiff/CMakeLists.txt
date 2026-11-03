# File: src/libs/Image.Codec.Vulkan.Tiff/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# Image.Codec.Vulkan.Tiff —— TIFF 编解码（平台无关）。
#
# 预测器（Predictor=2，水平差分）在 Vulkan compute；LZW 编解码在 CPU——
# 字典构建严格串行，GPU 版本只会更慢（见 tiff_codec.h 的说明）。
# 容器语义与 CUDA / OpenCL 后端一致。
set(VIC_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
set(VIC_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${VIC_SPV_DIR}" "${VIC_GEN_DIR}")

set(VIC_SPV_FILES "")
foreach(s tiff_predict)
  vic_add_shader_spv("${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/${s}.comp"
                     "${VIC_SPV_DIR}/${s}.spv")
  list(APPEND VIC_SPV_FILES "${VIC_SPV_DIR}/${s}.spv")
endforeach()

set(VIC_SPV_HEADER "${VIC_GEN_DIR}/tiff_predict_spv_data.h")
vic_spv_to_header("${VIC_SPV_FILES}" "${VIC_SPV_HEADER}")

add_library(Image.Codec.Vulkan.Tiff STATIC
  src/tiff_codec.h
  src/tiff_codec.cpp
  src/tiff_predict_vk.h
  src/tiff_predict_vk.cpp
  src/kernels/tiff_predict.comp
  "${VIC_SPV_HEADER}"
  ${VIC_SPV_FILES})

target_include_directories(Image.Codec.Vulkan.Tiff PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(Image.Codec.Vulkan.Tiff PRIVATE
  "${VIC_GEN_DIR}")

target_link_libraries(Image.Codec.Vulkan.Tiff PUBLIC
  Image.Codec.Vulkan::Common
  Vulkan::Vulkan)

add_library(Image.Codec.Vulkan::Tiff ALIAS Image.Codec.Vulkan.Tiff)
