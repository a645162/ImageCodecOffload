# File: src/libs/Image.Codec.Vulkan.Metrics/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# Image.Codec.Vulkan.Metrics —— MSE / PSNR / SSIM / MS-SSIM（平台无关）。
#
# 着色器在构建期由 glslc 或 glslangValidator 编成 SPIR-V，再转成 uint32 数组头
# 嵌进静态库：不提交 .spv，也不要求运行期存在 GLSL 编译器。
set(VIC_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
set(VIC_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${VIC_SPV_DIR}" "${VIC_GEN_DIR}")

set(VIC_SPV_FILES "")
foreach(s metrics)
  vic_add_shader_spv("${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/${s}.comp"
                     "${VIC_SPV_DIR}/${s}.spv")
  list(APPEND VIC_SPV_FILES "${VIC_SPV_DIR}/${s}.spv")
endforeach()

set(VIC_SPV_HEADER "${VIC_GEN_DIR}/metrics_spv_data.h")
vic_spv_to_header("${VIC_SPV_FILES}" "${VIC_SPV_HEADER}")

add_library(Image.Codec.Vulkan.Metrics STATIC
  src/metrics.cpp
  src/metrics_common.cpp
  src/metrics_cpu.cpp
  ${VIC_SPV_FILES}
  "${VIC_SPV_HEADER}")

target_include_directories(Image.Codec.Vulkan.Metrics PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(Image.Codec.Vulkan.Metrics PRIVATE
  "${VIC_GEN_DIR}")

target_link_libraries(Image.Codec.Vulkan.Metrics PUBLIC
  Image.Codec.Vulkan::Common
  Vulkan::Vulkan)

add_library(Image.Codec.Vulkan::Metrics ALIAS Image.Codec.Vulkan.Metrics)
