# File: examples/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

# 示例：纯 C 与 C++ 两种消费方式，均走对外 Image.Codec.OpenCL::All。
add_executable(image_codec_example_c c_example.c)
target_link_libraries(image_codec_example_c PRIVATE Image.Codec.OpenCL::All)
# 该可执行依赖 C++ 静态库，需用 C++ 链接器拉入 C++ 运行时。
set_target_properties(image_codec_example_c PROPERTIES
  C_STANDARD 11
  LINKER_LANGUAGE CXX)

add_executable(image_codec_example_cpp cpp_example.cpp)
target_link_libraries(image_codec_example_cpp PRIVATE Image.Codec.OpenCL::All)
set_target_properties(image_codec_example_cpp PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
