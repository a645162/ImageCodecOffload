# File: tests/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# 路由层的自检（返回码约定：0 = PASS、2 = SKIP、其它 = FAIL）。

add_executable(ico_test ico_test.cpp)
target_link_libraries(ico_test PRIVATE
  ImageCodecOffload.Api
  ImageCodecOffload.Router
  ImageCodecOffload.Cpu
  ImageCodecOffload.Common)

# 测试要拿内部接口（BackendKind / Router::encodeWith / metrics 参考实现）：
# 这些不在对外 C API 里，因此显式指到各库的 src/。
target_include_directories(ico_test PRIVATE
  "${PROJECT_SOURCE_DIR}/src/libs/ImageCodecOffload.Core/src"
  "${PROJECT_SOURCE_DIR}/src/libs/ImageCodecOffload.Common/src"
  "${PROJECT_SOURCE_DIR}/src/libs/ImageCodecOffload.Cpu/src"
  "${PROJECT_SOURCE_DIR}/src/libs/ImageCodecOffload.Router/src")

add_test(NAME ico_test COMMAND ico_test)
set_tests_properties(ico_test PROPERTIES SKIP_RETURN_CODE 2)
