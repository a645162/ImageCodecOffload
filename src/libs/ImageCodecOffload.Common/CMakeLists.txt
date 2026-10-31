# File: src/libs/ImageCodecOffload.Common/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

add_library(ImageCodecOffload.Common STATIC
  src/bmp_io.h
  src/bmp_io.cpp)

target_include_directories(ImageCodecOffload.Common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(ImageCodecOffload.Common PUBLIC
  ImageCodecOffload.Core)

add_library(ImageCodecOffload::Common ALIAS ImageCodecOffload.Common)
