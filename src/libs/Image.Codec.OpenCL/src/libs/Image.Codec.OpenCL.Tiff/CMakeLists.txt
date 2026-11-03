# File: src/libs/Image.Codec.OpenCL.Tiff/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

# TIFF 模块：容器读写 + LZW 编码(GPU)/解码(CPU)。

# ---- 内嵌 kernel 源码生成 C++ 字符串（file(READ) + configure_file）----
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/lzw_encode.cl" LZW_ENCODE_CL_STRING)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lzw_encode_cl.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/lzw_encode_cl.cpp"
  @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/lzw_encode_tile.cl" LZW_ENCODE_TILE_CL_STRING)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lzw_encode_tile_cl.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/lzw_encode_tile_cl.cpp"
  @ONLY)
# LZW 解码 GPU 探索 kernel（段级并行；见 lzw_decode_ocl.h 探索结论）。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/lzw_decode.cl" LZW_DECODE_CL_STRING)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lzw_decode_cl.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/lzw_decode_cl.cpp"
  @ONLY)
# LZW 解码 GPU 段内并行探索 kernel（见 lzw_decode_ocl.h 探索结论）。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/lzw_decode_par.cl" LZW_DECODE_PAR_CL_STRING)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lzw_decode_par_cl.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/lzw_decode_par_cl.cpp"
  @ONLY)

add_library(Image.Codec.OpenCL.Tiff STATIC
  src/tiff_container.h
  src/tiff_container.cpp
  src/lzw_encode_ocl.h
  src/lzw_encode_ocl.cpp
  src/lzw_decode_cpu.h
  src/lzw_decode_cpu.cpp
  src/lzw_decode_ocl.h
  src/lzw_decode_ocl.cpp
  src/lzw_codec.h
  src/lzw_codec.cpp
  src/tiff_test.h
  src/tiff_test.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/lzw_encode_cl.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/lzw_encode_tile_cl.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/lzw_decode_cl.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/lzw_decode_par_cl.cpp")

target_include_directories(Image.Codec.OpenCL.Tiff PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(Image.Codec.OpenCL.Tiff PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(Image.Codec.OpenCL.Tiff PUBLIC
  Image.Codec.OpenCL.Core
  Image.Codec.OpenCL.Common)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.OpenCL::Tiff)
add_library(Image.Codec.OpenCL::Tiff ALIAS Image.Codec.OpenCL.Tiff)
