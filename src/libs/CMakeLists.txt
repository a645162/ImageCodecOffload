# File: src/libs/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# 本项目的一等库（与主项目 src/libs 同样的组织方式）：每个目录一个静态库，
# 提供 Image.Codec.Offload::<Name> 别名，使用方按需链接。
add_subdirectory(Image.Codec.Offload.Core)   # 类型 / 错误 / 指标 CPU 参考 / 后端接口
add_subdirectory(Image.Codec.Offload.Common) # BMP 读写（CLI 与测试的中转格式）
add_subdirectory(Image.Codec.Offload.Cpu)    # libtiff / libpng / libjpeg-turbo 基线
add_subdirectory(Image.Codec.Offload.Router) # 后端选择与回退（含 OpenCL / CUDA 适配）
add_subdirectory(Image.Codec.Offload.Api)    # 对外 C API（image_codec_offload.h）
