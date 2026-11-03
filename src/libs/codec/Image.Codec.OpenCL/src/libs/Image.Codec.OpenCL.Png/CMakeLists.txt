# File: src/libs/Image.Codec.OpenCL.Png/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# PNG 模块：容器/CRC/zlib 在 CPU，行滤波（filter + 自适应选择）在 OpenCL。
#
# 为什么要 zlib 依赖：deflate 留在 CPU 是刻意的——这些尺寸下 GPU deflate 没有收益，
# 还会让「码流是否与 libpng 兼容」变成不可验证的问题。与 CUDA 后端保持同一分工。

# ---- 内嵌 OpenCL kernel 源码：file(READ) + configure_file 生成 C 字符串 ----
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/png_filter.cl" PNG_FILTER_SOURCE)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/cl_strings.cpp.in"
               "${CMAKE_CURRENT_BINARY_DIR}/png_cl_strings.cpp" @ONLY)

add_library(Image.Codec.OpenCL.Png STATIC
  src/png_container.h
  src/png_container.cpp
  src/png_filter_ocl.h
  src/png_filter_ocl.cpp
  src/png_codec.h
  src/png_codec.cpp
  src/png_test.h
  src/png_test.cpp
  src/kernels/png_filter.cl
  "${CMAKE_CURRENT_BINARY_DIR}/png_cl_strings.cpp")

target_include_directories(Image.Codec.OpenCL.Png PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(Image.Codec.OpenCL.Png PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}")

# Common 提供 ImageBuffer + BMP 中转；zlib 只在本模块（deflate/inflate）。
target_link_libraries(Image.Codec.OpenCL.Png PUBLIC
  Image.Codec.OpenCL.Core
  Image.Codec.OpenCL.Common)
find_package(ZLIB REQUIRED)
target_link_libraries(Image.Codec.OpenCL.Png PRIVATE ZLIB::ZLIB)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.OpenCL::Png)
add_library(Image.Codec.OpenCL::Png ALIAS Image.Codec.OpenCL.Png)
