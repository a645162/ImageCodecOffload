# File: src/libs/offload/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

# 路由层与 CPU 基线：始终加入，CPU 基线永远可用，同时充当 GPU 后端的像素级
# 参照。加入顺序即依赖顺序（Router 依赖 Core / Common / Cpu）。
add_subdirectory(Image.Codec.Offload.Core)   # 类型 / 错误 / 指标 CPU 参考 / 后端接口
add_subdirectory(Image.Codec.Offload.Common) # BMP 读写（CLI 与测试的中转格式）
add_subdirectory(Image.Codec.Offload.Cpu)    # libtiff / libpng / libjpeg-turbo 基线
add_subdirectory(Image.Codec.Offload.Router) # 后端选择与回退（含 OpenCL / CUDA 适配）
add_subdirectory(Image.Codec.Offload.Api)    # 对外 C API（image_codec_offload.h）
