# File: src/libs/Image.Codec.OpenCL.Common/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# Common：三个编解码模块共用的宿主内存像素容器与 BMP 中转读写。
#
# 为什么单独成库：TIFF / PNG / JPEG 都要「BMP 进、编码出」和「解码出、BMP 出」，
# 以前各自复制一份 bmp_io（细节还不一致）。统一到这里后，通道数（1 = 灰度、
# 3 = RGB）只有一个实现，通道保真也就只有一处需要验证。
add_library(Image.Codec.OpenCL.Common STATIC
  src/image_buffer.h
  src/bmp_io.h
  src/bmp_io.cpp)

target_include_directories(Image.Codec.OpenCL.Common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.OpenCL::Common)
add_library(Image.Codec.OpenCL::Common ALIAS Image.Codec.OpenCL.Common)
