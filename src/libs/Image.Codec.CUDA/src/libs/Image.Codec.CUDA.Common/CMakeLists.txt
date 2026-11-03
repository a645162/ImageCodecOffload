# File: src/libs/Image.Codec.CUDA.Common/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# 容器与像素的宿主侧实现（与 GPU 无关，可被任意后端复用）：
#   * image_buffer  —— 统一图像缓冲
#   * bmp_io        —— CLI/测试用 24-bit BMP
#   * tiff_container—— TIFF 6.0 容器（strip/tile 读写）
#   * lzw_codec     —— TIFF LZW CPU 参考实现（GPU kernel 的对拍基线）
#   * png_container —— PNG 容器 + 行滤波参考实现 + zlib deflate/inflate
add_library(Image.Codec.CUDA.Common STATIC
  src/image_buffer.h
  src/bmp_io.h
  src/bmp_io.cpp
  src/tiff_container.h
  src/tiff_container.cpp
  src/lzw_codec.h
  src/lzw_codec.cpp
  src/png_container.h
  src/png_container.cpp)

target_include_directories(Image.Codec.CUDA.Common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.CUDA.Common PUBLIC
  Image.Codec.CUDA::Core
  ZLIB::ZLIB)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.CUDA::Common)
add_library(Image.Codec.CUDA::Common ALIAS Image.Codec.CUDA.Common)
