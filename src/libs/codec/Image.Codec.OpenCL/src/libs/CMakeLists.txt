# File: src/libs/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

# libs/ 下为多个独立静态库项目（参照主项目 src/libs 组织），用户可按需链接。
# 每个库提供命名空间别名，例如：
#   target_link_libraries(app PRIVATE Image.Codec.OpenCL::Tiff)   # 仅 TIFF（PUBLIC 传播 Core）
#   target_link_libraries(app PRIVATE Image.Codec.OpenCL::Jpeg)   # 仅 JPEG（含 Core）
#   target_link_libraries(app PRIVATE Image.Codec.OpenCL::Png)   # 仅 PNG（含 Common/Core）
#   target_link_libraries(app PRIVATE Image.Codec.OpenCL::Metrics) # 仅画质指标
add_subdirectory(Image.Codec.OpenCL.Core)     # Image.Codec.OpenCL::Core       平台抽象 + OpenCL 设备/上下文/kernel
add_subdirectory(Image.Codec.OpenCL.Common)   # Image.Codec.OpenCL::Common     ImageBuffer + BMP 中转（1/3 通道）
add_subdirectory(Image.Codec.OpenCL.Tiff)     # Image.Codec.OpenCL::Tiff       TIFF 容器 + LZW 编码(GPU)/解码(CPU)
add_subdirectory(Image.Codec.OpenCL.Jpeg)     # Image.Codec.OpenCL::Jpeg       baseline JPEG 混合编解码（OpenCL）
add_subdirectory(Image.Codec.OpenCL.Png)      # Image.Codec.OpenCL::Png        PNG 容器 + GPU 行滤波
add_subdirectory(Image.Codec.OpenCL.Metrics)  # Image.Codec.OpenCL::Metrics    MSE/PSNR/SSIM/MS-SSIM（GPU + CPU 参考）
add_subdirectory(Image.Codec.OpenCL.Api)      # Image.Codec.OpenCL::Api        C API（image_codec.h）实现
