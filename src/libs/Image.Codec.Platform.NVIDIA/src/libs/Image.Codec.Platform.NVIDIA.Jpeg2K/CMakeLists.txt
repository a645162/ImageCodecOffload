# File: src/libs/Image.Codec.Platform.NVIDIA.Jpeg2K/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

# nvJPEG2000（JPEG 2000 / JP2）封装。nvJPEG2000 是独立安装的包，不走
# CUDA Toolkit 的导入目标——由顶层用 find_path / find_library 成对探测后
# 建出 NVIDIA::nvjpeg2k（见本工程顶层 CMakeLists.txt）。
add_library(Image.Codec.Platform.NVIDIA.Jpeg2K STATIC
  src/nvjpeg2k_codec.h
  src/nvjpeg2k_codec.cpp)

target_include_directories(Image.Codec.Platform.NVIDIA.Jpeg2K PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.Platform.NVIDIA.Jpeg2K PUBLIC
  Image.Codec.Platform.NVIDIA::Core
  NVIDIA::nvjpeg2k)

target_compile_definitions(Image.Codec.Platform.NVIDIA.Jpeg2K PRIVATE
  OIC_NVIDIA_HAS_NVJPEG2K=1)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.Platform.NVIDIA::Jpeg2K)
add_library(Image.Codec.Platform.NVIDIA::Jpeg2K ALIAS Image.Codec.Platform.NVIDIA.Jpeg2K)
