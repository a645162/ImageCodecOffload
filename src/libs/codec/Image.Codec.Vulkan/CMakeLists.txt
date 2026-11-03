# File: CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

cmake_minimum_required(VERSION 3.16)
project(Image.Codec.Vulkan LANGUAGES CXX)

# Keep all exported include paths rooted at this repository when the codec is
# consumed with add_subdirectory() from an application such as ImageViewerQt.
set(VIC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- vcpkg toolchain bootstrap（与 Image.Codec.OpenCL 相同模式）----
if(WIN32)
  set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet" FORCE)
endif()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
  elseif(EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
  endif()
endif()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(WARNING
    "vcpkg toolchain not located. Pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake "
    "or set env VCPKG_ROOT.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8)
endif()

# VK_USE_PLATFORM_WIN32_KHR 必须在**任何** <vulkan/vulkan.h> 之前定义，
# vulkan.h 才会把 vulkan_win32.h 引进来（PFN_vkGetMemoryWin32HandleKHR 等
# Win32 句柄类型都在那个头里）。vic.hpp 里虽然也定义了它，但只要有某个 TU
# 先包含了 <vulkan/vulkan.h>（vk_buffer.h / vk_pipeline.h 的历史包含顺序就是
# 这样），vulkan.h 的 include guard 已经生效，后面再定义就完全来不及，表现
# 为 context.h 里 PFN_vkGetMemoryWin32HandleKHR 报「unknown type name」。
#
# 这种事靠每个头自己摆顺序防不住（新增一个头就可能再踩一次），所以目录级
# 给所有目标（含测试与 examples）统一注入；Linux 没有 vulkan_win32.h，不能
# 无条件加，否则连 Vulkan 头都包含不进来。
if(WIN32)
  add_compile_definitions(VK_USE_PLATFORM_WIN32_KHR=1)
  # windows.h（vulkan_win32.h 会把它拉进来）默认定义 min/max 宏，会把源码里的
  # std::min/std::max 展开成 `std::(((a)<(b))?(a):(b))`，直接报「expected
  # unqualified-id」。整个项目有十几处 std::min 调用，逐个加括号只会等着下一次
  # 再踩，故统一关掉这两个宏。
  add_compile_definitions(NOMINMAX)
endif()

option(VIC_BUILD_TESTS "Build ctest suite" OFF)
option(VIC_BUILD_EXAMPLES "Build examples" OFF)

# Vulkan 由 vcpkg vulkan port 提供（headers + loader）。
find_package(Vulkan REQUIRED)
find_package(glslang CONFIG REQUIRED)

# vcpkg 的 glslang port提供库而非 glslangValidator 可执行文件。用项目自己的
# 构建期小工具调用同一套 API，避免把开发机上的 Vulkan SDK 命令行工具变成
# 隐含依赖。
add_executable(vic_glslang_compile EXCLUDE_FROM_ALL
  cmake/vic_glslang_compile.cpp)
target_compile_features(vic_glslang_compile PRIVATE cxx_std_17)
target_link_libraries(vic_glslang_compile PRIVATE
  glslang::glslang
  glslang::SPIRV
  glslang::glslang-default-resource-limits)

# ---- GLSL -> SPIR-V ----
# Vulkan SDK 带 glslc；Linux 发行版通常只提供 glslang-tools（glslangValidator）。
# 两者都能出 SPIR-V，命令行只差 -o 的位置，这里统一抹平——否则「Linux 上连 Core
# 都 configure 不过去」，而编解码与指标本身一行平台代码都没有。
find_program(VIC_GLSLC NAMES glslc
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
  PATH_SUFFIXES Bin bin)
if(NOT VIC_GLSLC AND TARGET Vulkan::glslc)
  get_target_property(VIC_GLSLC Vulkan::glslc IMPORTED_LOCATION)
endif()

find_program(VIC_GLSLANG_VALIDATOR NAMES glslangValidator glslang
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
  PATH_SUFFIXES Bin bin)

if(VIC_GLSLC)
  set(VIC_SHADER_COMPILER "${VIC_GLSLC}")
  set(VIC_SHADER_MODE glslc)
elseif(VIC_GLSLANG_VALIDATOR)
  set(VIC_SHADER_COMPILER "${VIC_GLSLANG_VALIDATOR}")
  set(VIC_SHADER_MODE glslang)
else()
  set(VIC_SHADER_MODE glslang-library)
  set(VIC_SHADER_COMPILER vic_glslang_compile)
endif()
message(STATUS "VIC shader compiler: ${VIC_SHADER_COMPILER} (${VIC_SHADER_MODE})")

# 把 .comp 编译成 .spv。生成物是构建产物，不提交进仓库。
function(vic_add_shader_spv shader out_spv)
  if(VIC_SHADER_MODE STREQUAL "glslc")
    add_custom_command(
      OUTPUT "${out_spv}"
      COMMAND "${VIC_SHADER_COMPILER}" "${shader}" -o "${out_spv}"
      DEPENDS "${shader}"
      VERBATIM)
  else()
    if(VIC_SHADER_MODE STREQUAL "glslang")
      add_custom_command(
        OUTPUT "${out_spv}"
        COMMAND "${VIC_SHADER_COMPILER}" -V "${shader}" -o "${out_spv}"
        DEPENDS "${shader}"
        VERBATIM)
    else()
      add_custom_command(
        OUTPUT "${out_spv}"
        COMMAND $<TARGET_FILE:vic_glslang_compile> "${shader}" "${out_spv}"
        DEPENDS "${shader}" vic_glslang_compile
        VERBATIM)
    endif()
  endif()
endfunction()

# 一组 .spv -> 一个 uint32_t 数组头（供 vkCreateShaderModule 直接使用）。
function(vic_spv_to_header spv_files out_header)
  add_custom_command(
    OUTPUT "${out_header}"
    COMMAND ${CMAKE_COMMAND}
      "-DSPV_FILES=${spv_files}"
      "-DOUT_HEADER=${out_header}"
      -P "${VIC_SOURCE_DIR}/cmake/spv_to_header.cmake"
    DEPENDS ${spv_files}
    VERBATIM)
endfunction()

add_subdirectory(src)
if(VIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
