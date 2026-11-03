# File: CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

cmake_minimum_required(VERSION 3.16)
project(Image.Codec.Platform.Intel LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8)
endif()
option(OIC_BUILD_CLI "Build test CLI" OFF)
add_subdirectory(src)
