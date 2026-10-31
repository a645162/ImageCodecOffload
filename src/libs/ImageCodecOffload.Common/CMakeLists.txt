add_library(ImageCodecOffload.Common STATIC
  src/bmp_io.h
  src/bmp_io.cpp)

target_include_directories(ImageCodecOffload.Common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(ImageCodecOffload.Common PUBLIC
  ImageCodecOffload.Core)

add_library(ImageCodecOffload::Common ALIAS ImageCodecOffload.Common)
