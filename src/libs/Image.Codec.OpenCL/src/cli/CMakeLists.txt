# File: src/cli/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

add_executable(image_codec main.cpp)
target_link_libraries(image_codec PRIVATE
  Image.Codec.OpenCL::All)
