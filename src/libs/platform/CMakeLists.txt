# File: src/libs/platform/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

# 厂商平台层：与 codec/ 下的通用 GPU 后端并列，但**不进路由后端表**
# （路由层看不到它们，父项目用 IMAGECODEC_OFFLOAD_WITH_PLATFORM /
# _WITH_NVIDIA_PLATFORM 单独挂载）。两者都默认 OFF。

# Image.Codec.Platform.Intel：Windows / Linux 通用的平台抽象 + Intel 媒体引擎
# （oneVPL）硬件 JPEG + TIFF(JPEG strips) 容器。依赖 libvpl 头文件子模块，
# 且只有 Intel 核显机器才用得上。
if(IMAGECODEC_OFFLOAD_WITH_PLATFORM)
  add_subdirectory(Image.Codec.Platform.Intel)
endif()

# Image.Codec.Platform.NVIDIA：NVIDIA 厂商编解码（nvJPEG / nvJPEG2000 / nvTIFF）。
# nvJPEG2000 与 nvTIFF 是独立安装的包（不是 CUDA Toolkit 的一部分），没有的
# 机器上配置期会打印一行 STATUS 并把对应编解码标记为不可用。
if(IMAGECODEC_OFFLOAD_WITH_NVIDIA_PLATFORM)
  add_subdirectory(Image.Codec.Platform.NVIDIA)
endif()
