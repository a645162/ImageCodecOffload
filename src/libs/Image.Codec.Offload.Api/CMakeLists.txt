# File: src/libs/Image.Codec.Offload.Api/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# 对外 C API（include/Image/Codec/Offload/image_codec_offload.h）实现库。
add_library(Image.Codec.Offload.Api STATIC
  src/image_codec_offload.cpp
  "${PROJECT_SOURCE_DIR}/include/Image/Codec/Offload/image_codec_offload.h")

target_include_directories(Image.Codec.Offload.Api PUBLIC
  "${PROJECT_SOURCE_DIR}/include")

target_link_libraries(Image.Codec.Offload.Api PUBLIC
  Image.Codec.Offload.Router)

# 共享库场景下 ICO_API 走 dllexport 分支；静态库下为空。
target_compile_definitions(Image.Codec.Offload.Api PRIVATE
  IMAGECODEC_OFFLOAD_BUILDING_LIB)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.Offload::Api)
add_library(Image.Codec.Offload::Api ALIAS Image.Codec.Offload.Api)
