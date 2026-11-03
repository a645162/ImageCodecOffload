# File: src/libs/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# libs/ 下为多个独立静态库，各自一个目录、各自一个命名空间别名：
#   Core   —— CUDA 设备/流/缓冲 RAII 与错误上下文
#   Common —— 容器与像素的宿主侧实现（BMP/TIFF 容器、PNG 容器、LZW 解码、zlib 封装）
#   Tiff   —— TIFF LZW 编码（CUDA kernel）+ 解码（CPU，容器语义对齐 libtiff）
#   NvJpeg —— NVIDIA 固定功能硬件编解码（nvJPEG / NVJPG 单元）
#   Png    —— PNG 编码（GPU 行滤波）+ 解码（CPU inflate + unfilter）
#   Api    —— 对外稳定 C API（image_codec_cuda.h）实现
#   Metrics —— 画质指标（MSE/PSNR/SSIM，GPU 并行 + CPU 参考）
#   Tune   —— 参数扫描库（在自己硬件上找最优 tile/批大小/线程数）
#
# 厂商硬件路径的分工：NVIDIA 的 nvJPEG/NVJPG 属于本仓库（CUDA Toolkit 自带组件），
# Intel 的 oneVPL/QuickSync 属于 Image.Codec.Platform.Intel 的对应模块；两者都只在能力表里
# 报告「真的可用」时才会被上层路由到。
add_subdirectory(Image.Codec.CUDA.Core)    # Image.Codec.CUDA::Core
add_subdirectory(Image.Codec.CUDA.Common)  # Image.Codec.CUDA::Common
add_subdirectory(Image.Codec.CUDA.Tiff)    # Image.Codec.CUDA::Tiff
add_subdirectory(Image.Codec.CUDA.NvJpeg)  # Image.Codec.CUDA::NvJpeg
add_subdirectory(Image.Codec.CUDA.Png)     # Image.Codec.CUDA::Png
add_subdirectory(Image.Codec.CUDA.Api)     # Image.Codec.CUDA::Api
add_subdirectory(Image.Codec.CUDA.Metrics)  # Image.Codec.CUDA::Metrics
add_subdirectory(Image.Codec.CUDA.Tune)    # Image.Codec.CUDA::Tune
