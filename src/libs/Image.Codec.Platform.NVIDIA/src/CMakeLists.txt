# File: src/libs/Image.Codec.Platform.NVIDIA/src/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

add_subdirectory(libs)

# 自测 CLI 默认不构建（它不是库的一部分，只用于本机验证厂商库与往返）。
if(OIC_BUILD_CLI)
  add_subdirectory(cli)
endif()
