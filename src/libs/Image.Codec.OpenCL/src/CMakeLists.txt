# File: src/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

# src/ 下为多个独立静态库项目（参照主项目 src/libs 组织），用户可按需链接。
# 每个库提供命名空间别名，例如：
#   target_link_libraries(app PRIVATE Image.Codec.OpenCL::Tiff)   # 仅 TIFF（PUBLIC 传播 Core）
#   target_link_libraries(app PRIVATE Image.Codec.OpenCL::Jpeg)   # 仅 JPEG（含 Core）
add_subdirectory(libs)   # libs/ 下 5 个独立静态库（见 libs/CMakeLists.txt，用户可按需链接）

# 全量聚合便捷入口（可选）：一次链接全部库。不需要全部时可省略。
add_library(Image.Codec.OpenCL INTERFACE)
target_include_directories(Image.Codec.OpenCL INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>")
target_link_libraries(Image.Codec.OpenCL INTERFACE
  Image.Codec.OpenCL::Core
  Image.Codec.OpenCL::Tiff
  Image.Codec.OpenCL::Jpeg
  Image.Codec.OpenCL::Api)
add_library(Image.Codec.OpenCL::All ALIAS Image.Codec.OpenCL)
