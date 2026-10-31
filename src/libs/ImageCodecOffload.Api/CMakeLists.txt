# 对外 C API（include/ImageCodecOffload/image_codec_offload.h）实现库。
add_library(ImageCodecOffload.Api STATIC
  src/image_codec_offload.cpp
  "${PROJECT_SOURCE_DIR}/include/ImageCodecOffload/image_codec_offload.h")

target_include_directories(ImageCodecOffload.Api PUBLIC
  "${PROJECT_SOURCE_DIR}/include")

target_link_libraries(ImageCodecOffload.Api PUBLIC
  ImageCodecOffload.Router)

# 共享库场景下 ICO_API 走 dllexport 分支；静态库下为空。
target_compile_definitions(ImageCodecOffload.Api PRIVATE
  IMAGECODEC_OFFLOAD_BUILDING_LIB)

# 命名空间别名：target_link_libraries(app PRIVATE ImageCodecOffload::Api)
add_library(ImageCodecOffload::Api ALIAS ImageCodecOffload.Api)
