# File: src/libs/Image.Codec.Platform.Intel.Tiff/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# TIFF 6.0 容器写出：把预编码的 JPEG strips（Compression=7）或未压缩
# RGB strips（Compression=1）写进 TIFF。
#
# 本项目不依赖 libtiff 写文件：libtiff 的 JPEG codec 只会拿像素重新编码，
# 没有入口写进硬件已经编好的码流。libtiff 仅用于测试端解码校验
# （见 src/cli 的自测程序）。
add_library(Image.Codec.Platform.Intel.Tiff STATIC
  src/jpeg_stream.cpp
  src/tiff_jpeg_strip_writer.cpp)

target_include_directories(Image.Codec.Platform.Intel.Tiff PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(Image.Codec.Platform.Intel::Tiff ALIAS Image.Codec.Platform.Intel.Tiff)
