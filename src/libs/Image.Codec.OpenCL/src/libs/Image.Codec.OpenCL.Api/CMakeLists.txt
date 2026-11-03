# File: src/libs/Image.Codec.OpenCL.Api/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

# 对外 C API（include/Image/Codec/OpenCL/image_codec.h / image_codec.hpp 的实现库）。
# 桥接各模块文件入口（image_codec_*_test.h）与内存 buffer。
add_library(Image.Codec.OpenCL.Api STATIC
  src/image_codec.cpp
  "${PROJECT_SOURCE_DIR}/include/Image/Codec/OpenCL/image_codec.h"
  "${PROJECT_SOURCE_DIR}/include/Image/Codec/OpenCL/image_codec.hpp")

target_include_directories(Image.Codec.OpenCL.Api PUBLIC
  "${PROJECT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(Image.Codec.OpenCL.Api PUBLIC
  Image.Codec.OpenCL.Core
  Image.Codec.OpenCL.Common
  Image.Codec.OpenCL.Tiff
  Image.Codec.OpenCL.Jpeg
  Image.Codec.OpenCL.Png
  Image.Codec.OpenCL.Metrics)
# 构建本库时 IMAGE_CODEC_API 取 dllexport 分支（共享库场景）；静态库下为空。
target_compile_definitions(Image.Codec.OpenCL.Api PRIVATE IMAGE_CODEC_BUILDING_LIB)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.OpenCL::Api)
add_library(Image.Codec.OpenCL::Api ALIAS Image.Codec.OpenCL.Api)
