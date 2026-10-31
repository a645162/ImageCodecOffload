# File: src/cli/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

add_executable(ico main.cpp)

target_link_libraries(ico PRIVATE
  ImageCodecOffload.Api
  ImageCodecOffload.Common)

# CLI 需要拿到内部接口（Router 的强制后端入口与 backendName）：这些不属于
# 对外 C API，故意不放进 include/ 的公共头里。
target_include_directories(ico PRIVATE
  "${PROJECT_SOURCE_DIR}/src/libs/ImageCodecOffload.Router/src"
  "${PROJECT_SOURCE_DIR}/src/libs/ImageCodecOffload.Core/src")
