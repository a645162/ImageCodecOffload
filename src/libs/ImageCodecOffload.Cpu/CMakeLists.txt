add_library(ImageCodecOffload.Cpu STATIC
  src/tiff_io.h
  src/tiff_io.cpp
  src/png_io.h
  src/png_io.cpp
  src/jpeg_io.h
  src/jpeg_io.cpp
  src/container_probe.h
  src/container_probe.cpp
  src/cpu_backend.h
  src/cpu_backend.cpp)

target_include_directories(ImageCodecOffload.Cpu PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

# 用 TIFF::TIFF 这类导入目标而不是裸库名：vcpkg 与系统包给的
# include/lib 路径不同（Debian 的 tiffio.h 就在 x86_64-linux-gnu/ 下）。
target_link_libraries(ImageCodecOffload.Cpu PUBLIC
  ImageCodecOffload.Core
  TIFF::TIFF
  PNG::PNG
  JPEG::JPEG)

add_library(ImageCodecOffload::Cpu ALIAS ImageCodecOffload.Cpu)
