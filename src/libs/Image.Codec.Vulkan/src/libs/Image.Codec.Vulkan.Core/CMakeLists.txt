# File: src/libs/Image.Codec.Vulkan.Core/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

# Image.Codec.Vulkan.Core —— 设备/上下文、可导出资源、compute pipeline（平台无关）。
#
# 着色器编译工具链的探测与 vic_add_shader_spv() 在顶层 CMakeLists.txt 里，
# 各模块（Core / Metrics / Png / Tiff / Jpeg）共用同一套 glslc / glslangValidator 规则。

set(VIC_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
set(VIC_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(VIC_SPV_HEADER "${VIC_GEN_DIR}/vic_spv_data.h")
file(MAKE_DIRECTORY "${VIC_SPV_DIR}" "${VIC_GEN_DIR}")

set(VIC_SHADERS gradient_buffer gradient_texture)
set(VIC_SPV_FILES "")
foreach(s ${VIC_SHADERS})
  vic_add_shader_spv("${CMAKE_CURRENT_SOURCE_DIR}/shaders/${s}.comp"
                     "${VIC_SPV_DIR}/${s}.spv")
  list(APPEND VIC_SPV_FILES "${VIC_SPV_DIR}/${s}.spv")
endforeach()

vic_spv_to_header("${VIC_SPV_FILES}" "${VIC_SPV_HEADER}")

add_library(Image.Codec.Vulkan.Core STATIC
  src/context.cpp
  src/exportable.cpp
  src/compute.cpp
  # 生成的头必须列进源码：否则编译器先跑去编 compute.cpp，
  # 自定义命令还没执行，头文件尚不存在（并行构建下必现）。
  "${VIC_SPV_HEADER}"
  ${VIC_SPV_FILES})
target_include_directories(Image.Codec.Vulkan.Core PUBLIC
  "${VIC_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${VIC_GEN_DIR}"
  # vic.hpp 为 umbrella 头，Core/GlInterop/Api 的 .cpp 内部也经它编译；
  # 故把兄弟库源码目录也纳入 PUBLIC include（GlInterop/Api 经 PUBLIC 链接 Core
  # 自动继承，消费者经 Api 同样获得全部目录）。
  "${VIC_SOURCE_DIR}/src/libs/Image.Codec.Vulkan.GlInterop/src"
  "${VIC_SOURCE_DIR}/src/libs/Image.Codec.Vulkan.Api/src")
target_link_libraries(Image.Codec.Vulkan.Core PUBLIC Vulkan::Vulkan)
add_library(Image.Codec.Vulkan::Core ALIAS Image.Codec.Vulkan.Core)
