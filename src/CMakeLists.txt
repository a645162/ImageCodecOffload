# File: src/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# src/ 下是本项目自己实现的代码：路由层、CPU 基线、C API、CLI，以及
# `src/libs/` 下的全部后端实现（OpenCL / CUDA / Vulkan / Platform.Intel，见顶层
# CMakeLists 的 add_subdirectory）。后端先于路由层加入，路由层才能按 TARGET 判断
# 某个后端是否真的编进了本次构建。

add_subdirectory(libs)

if(IMAGECODEC_OFFLOAD_BUILD_CLI)
  add_subdirectory(cli)
endif()
