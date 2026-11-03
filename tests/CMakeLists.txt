# File: tests/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# 路由层的自检（返回码约定：0 = PASS、2 = SKIP、其它 = FAIL）。

add_executable(ico_test ico_test.cpp)
target_link_libraries(ico_test PRIVATE
  Image.Codec.Offload.Api
  Image.Codec.Offload.Router
  Image.Codec.Offload.Cpu
  Image.Codec.Offload.Common)

# 测试要拿内部接口（BackendKind / Router::encodeWith / metrics 参考实现）：
# 这些不在对外 C API 里，因此显式指到各库的 src/。
target_include_directories(ico_test PRIVATE
  "${PROJECT_SOURCE_DIR}/src/libs/offload/Image.Codec.Offload.Core/src"
  "${PROJECT_SOURCE_DIR}/src/libs/offload/Image.Codec.Offload.Common/src"
  "${PROJECT_SOURCE_DIR}/src/libs/offload/Image.Codec.Offload.Cpu/src"
  "${PROJECT_SOURCE_DIR}/src/libs/offload/Image.Codec.Offload.Router/src")

# CPU 目标的可选依赖宏是 PRIVATE；测试需要同一事实来断言能力页里的
# ExecutionMode，不能只看全局 option（option 打开但找不到 port 时仍不可用）。
if(IMAGECODEC_OFFLOAD_WITH_JXL AND TARGET PkgConfig::JXL_PC)
  target_compile_definitions(ico_test PRIVATE IMAGECODEC_OFFLOAD_WITH_JXL=1)
endif()
if(IMAGECODEC_OFFLOAD_WITH_OPENJPEG AND
   (TARGET OpenJPEG::OpenJPEG OR TARGET openjp2_static OR TARGET openjp2 OR
    TARGET PkgConfig::OPENJPEG_PC))
  target_compile_definitions(ico_test PRIVATE IMAGECODEC_OFFLOAD_WITH_OPENJPEG=1)
endif()

add_test(NAME ico_test COMMAND ico_test)
set_tests_properties(ico_test PROPERTIES SKIP_RETURN_CODE 2)
