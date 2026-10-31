# File: src/libs/ImageCodecOffload.Cpu/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

add_library(ImageCodecOffload.Cpu STATIC
  src/tiff_io.h
  src/tiff_io.cpp
  src/tiff_container.h
  src/tiff_container.cpp
  src/png_io.h
  src/png_io.cpp
  src/jpeg_io.h
  src/jpeg_io.cpp
  src/openjpeg_io.h
  src/openjpeg_io.cpp
  src/jpegxl_io.h
  src/jpegxl_io.cpp
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

if(IMAGECODEC_OFFLOAD_WITH_OPENJPEG AND TARGET OpenJPEG::OpenJPEG)
  target_link_libraries(ImageCodecOffload.Cpu PUBLIC OpenJPEG::OpenJPEG)
  target_compile_definitions(ImageCodecOffload.Cpu PRIVATE IMAGECODEC_OFFLOAD_WITH_OPENJPEG=1)
elseif(IMAGECODEC_OFFLOAD_WITH_OPENJPEG AND (TARGET openjp2_static OR TARGET openjp2))
  target_link_libraries(ImageCodecOffload.Cpu PUBLIC $<IF:$<TARGET_EXISTS:openjp2_static>,openjp2_static,openjp2>)
  target_compile_definitions(ImageCodecOffload.Cpu PRIVATE IMAGECODEC_OFFLOAD_WITH_OPENJPEG=1)
elseif(IMAGECODEC_OFFLOAD_WITH_OPENJPEG AND TARGET PkgConfig::OPENJPEG_PC)
  target_link_libraries(ImageCodecOffload.Cpu PUBLIC PkgConfig::OPENJPEG_PC)
  target_compile_definitions(ImageCodecOffload.Cpu PRIVATE IMAGECODEC_OFFLOAD_WITH_OPENJPEG=1)
endif()

if(IMAGECODEC_OFFLOAD_WITH_JXL AND TARGET PkgConfig::JXL_PC)
  target_link_libraries(ImageCodecOffload.Cpu PUBLIC PkgConfig::JXL_PC)
  target_compile_definitions(ImageCodecOffload.Cpu PRIVATE IMAGECODEC_OFFLOAD_WITH_JXL=1)
endif()

add_library(ImageCodecOffload::Cpu ALIAS ImageCodecOffload.Cpu)
