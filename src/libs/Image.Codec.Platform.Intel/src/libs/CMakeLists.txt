# File: src/libs/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

# 平台与 Intel 专用库（参照 Image.Codec.Offload 组织，可按需链接）。
add_subdirectory(Image.Codec.Platform.Intel.Core)     # Image.Codec.Platform.Intel::Core        平台抽象（exe_dir/计时/休眠）
add_subdirectory(Image.Codec.Platform.Intel.Jpeg.oneVPL)  # Image.Codec.Platform.Intel::Jpeg::oneVPL    oneVPL 硬件 JPEG（依赖 Platform）
add_subdirectory(Image.Codec.Platform.Intel.Tiff)         # Image.Codec.Platform.Intel::Tiff             TIFF 6.0 容器（JPEG strips / 未压缩）
