# File: tests/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# OpenCL 后端的自检（返回码约定：0 = PASS、2 = SKIP、其它 = FAIL）。

add_executable(metrics_test metrics_test.cpp)
target_link_libraries(metrics_test PRIVATE
  Image.Codec.OpenCL::Metrics)
# 头文件在模块 src/ 下（内部接口），测试与库共用同一份定义。
target_include_directories(metrics_test PRIVATE
  "${PROJECT_SOURCE_DIR}/src/libs/Image.Codec.OpenCL.Metrics/src")

add_test(NAME metrics_test COMMAND metrics_test)
# SKIP（无设备 / 不支持 fp64）不算失败，但也不是「通过」——ctest 用 SKIP_RETURN_CODE 区分。
set_tests_properties(metrics_test PROPERTIES SKIP_RETURN_CODE 2)

# JPEG 混合编解码：子采样/容器自洽、通道保真、GPU 与 CPU 参考解码一致。
add_executable(jpeg_codec_test jpeg_codec_test.cpp)
target_link_libraries(jpeg_codec_test PRIVATE Image.Codec.OpenCL::Jpeg)
# 头文件在模块 src/ 下（内部接口），测试与库共用同一份定义。
target_include_directories(jpeg_codec_test PRIVATE
  "${PROJECT_SOURCE_DIR}/src/libs/Image.Codec.OpenCL.Jpeg/src")

add_test(NAME jpeg_codec_test COMMAND jpeg_codec_test)
set_tests_properties(jpeg_codec_test PROPERTIES SKIP_RETURN_CODE 2)
