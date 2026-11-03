# File: src/libs/Image.Codec.OpenCL.Jpeg/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

# JPEG 模块：OpenCL 混合编解码（Huffman 熵编解码 CPU + DCT/颜色变换 GPU）。

# ---- 内嵌 OpenCL kernel 源码：file(READ) + configure_file 生成 C 字符串 ----
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/jpeg_dec.cl" JPEG_DEC_SOURCE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/jpeg_enc.cl" JPEG_ENC_SOURCE)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/cl_strings.cpp.in"
               "${CMAKE_CURRENT_BINARY_DIR}/jpeg_cl_strings.cpp" @ONLY)

add_library(Image.Codec.OpenCL.Jpeg STATIC
  src/jpeg_bitstream.h
  src/jpeg_bitstream.cpp
  src/jpeg_huffman.h
  src/jpeg_huffman.cpp
  src/jpeg_dct_ocl.h
  src/jpeg_dct_ocl.cpp
  src/jpeg_codec.h
  src/jpeg_codec.cpp
  src/jpeg_test.h
  src/jpeg_test.cpp
  src/kernels/jpeg_dec.cl
  src/kernels/jpeg_enc.cl
  "${CMAKE_CURRENT_BINARY_DIR}/jpeg_cl_strings.cpp")
target_include_directories(Image.Codec.OpenCL.Jpeg PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.OpenCL.Jpeg PUBLIC
  Image.Codec.OpenCL.Core
  Image.Codec.OpenCL.Common)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.OpenCL::Jpeg)
add_library(Image.Codec.OpenCL::Jpeg ALIAS Image.Codec.OpenCL.Jpeg)

# 可选自测可执行（默认关闭；供本模块独立验证）。
option(IMAGE_CODEC_BUILD_JPEG_SELFTEST "Build JPEG self-test executable" OFF)
if(IMAGE_CODEC_BUILD_JPEG_SELFTEST)
  add_executable(image_codec_jpeg_selftest src/selftest/selftest.cpp)
  target_link_libraries(image_codec_jpeg_selftest PRIVATE Image.Codec.OpenCL.Jpeg)
endif()
