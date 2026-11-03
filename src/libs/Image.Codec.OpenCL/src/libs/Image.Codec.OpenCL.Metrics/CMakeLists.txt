# File: src/libs/Image.Codec.OpenCL.Metrics/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# 画质指标模块：MSE / PSNR / SSIM / MS-SSIM（OpenCL GPU + CPU 参考实现）。

# ---- 内嵌 kernel 源码生成 C++ 字符串（file(READ) + configure_file）----
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/metrics.cl" METRICS_CL_STRING)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/metrics_cl.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/metrics_cl.cpp"
  @ONLY)

add_library(Image.Codec.OpenCL.Metrics STATIC
  src/metrics.h
  src/metrics_common.h
  src/metrics_common.cpp
  src/metrics_cpu.cpp
  src/metrics.cpp
  src/kernels/metrics.cl
  "${CMAKE_CURRENT_BINARY_DIR}/metrics_cl.cpp")

target_include_directories(Image.Codec.OpenCL.Metrics PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(Image.Codec.OpenCL.Metrics PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(Image.Codec.OpenCL.Metrics PUBLIC
  Image.Codec.OpenCL.Core)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.OpenCL::Metrics)
add_library(Image.Codec.OpenCL::Metrics ALIAS Image.Codec.OpenCL.Metrics)
