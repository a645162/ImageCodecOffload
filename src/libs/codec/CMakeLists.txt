# File: src/libs/codec/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

# 通用 GPU 计算后端：三家各自实现同一套编解码能力，由能力表在路由层选择。
# 三个开关默认 OFF（OpenCL 除外），因为后两者要求额外的工具链 / 运行库。

# OpenCL 覆盖率最广（Intel/NVIDIA/AMD），且不要求额外工具链，因此默认打开。
if(IMAGECODEC_OFFLOAD_WITH_OPENCL)
  add_subdirectory(Image.Codec.OpenCL)
endif()

if(IMAGECODEC_OFFLOAD_WITH_CUDA)
  add_subdirectory(Image.Codec.CUDA)
endif()

# Vulkan 后端：TIFF / PNG 与画质指标已实现；JPEG 尚未实现，由能力表
# （capabilities.jpeg_* = 0）在选后端时挡掉，交给 OpenCL / CUDA / CPU。
if(IMAGECODEC_OFFLOAD_WITH_VULKAN)
  add_subdirectory(Image.Codec.Vulkan)
endif()
