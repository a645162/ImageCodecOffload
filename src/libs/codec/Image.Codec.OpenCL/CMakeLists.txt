# File: CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

cmake_minimum_required(VERSION 3.16)
project(Image.Codec.OpenCL LANGUAGES C CXX)

# ---- vcpkg toolchain bootstrap ----
# Manifest-mode: 本仓库自带 vcpkg.json，dependencies=[opencl]。
# 独立构建时若无 CMAKE_TOOLCHAIN_FILE，自动探测 VCPKG_ROOT / C:/vcpkg。
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

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码为 UTF-8（含中文注释）；MSVC 默认按系统代码页解析会乱码并破坏词法。
if(MSVC)
  add_compile_options(/utf-8)
endif()

option(IMAGE_CODEC_BUILD_CLI "Build test CLI" ON)
option(IMAGE_CODEC_BUILD_TESTS "Build ctest suite" OFF)
option(IMAGE_CODEC_BUILD_EXAMPLES "Build examples" ON)

# OpenCL 由 vcpkg opencl 端口提供（OpenCL::OpenCL）。
find_package(OpenCL REQUIRED)

add_subdirectory(src)
if(IMAGE_CODEC_BUILD_CLI)
  add_subdirectory(src/cli)
endif()
if(IMAGE_CODEC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(IMAGE_CODEC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
