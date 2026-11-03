# File: src/libs/Image.Codec.Offload.Common/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

add_library(Image.Codec.Offload.Common STATIC
  src/bmp_io.h
  src/bmp_io.cpp)

target_include_directories(Image.Codec.Offload.Common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.Offload.Common PUBLIC
  Image.Codec.Offload.Core)

add_library(Image.Codec.Offload::Common ALIAS Image.Codec.Offload.Common)
