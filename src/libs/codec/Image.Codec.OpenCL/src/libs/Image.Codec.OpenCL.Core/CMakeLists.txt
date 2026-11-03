# File: src/libs/Image.Codec.OpenCL.Core/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

# 核心基础设施：平台抽象（exe_dir/计时/休眠）+ OpenCL 设备/上下文/kernel 构建。
add_library(Image.Codec.OpenCL.Core STATIC
  src/image_codec_platform.h
  src/win_platform.cpp
  src/ocl_device.h
  src/ocl_device.cpp
  src/ocl_program.h
  src/ocl_program.cpp)
target_include_directories(Image.Codec.OpenCL.Core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(Image.Codec.OpenCL.Core PUBLIC OpenCL::OpenCL)

# CL 头的声明面定到 OpenCL 1.2：本库全部 kernel 都以 "-cl-std=CL1.2" 构建，host 侧
# 也只用 1.x 入口（clCreateCommandQueue、clCreateFromGLTexture …）。
# 不定义这个宏时 CL/cl_version.h 只打一句 "Defaulting to 300 (OpenCL 3.0)" 的
# #pragma message 就继续编译，于是 3.0 才有的入口（clCreateBufferWithProperties、
# clSetProgramSpecializationConstant、clSetContextDestructorCallback …）全部变成"可见"，
# 而 AMD 的 GPU 运行时最高只声称 2.0/2.1、根本不导出它们——那类调用会在链接期或
# 运行期才失败。定到 120 让编译器提前拦住。
# 用 PUBLIC：所有 Image.Codec.OpenCL.* 与路由层的 OpenCL TU 都要继承同一份声明面，
# 各库各写一份必然漂移。1.2 已由 cl_version.h 自动打开
# CL_USE_DEPRECATED_OPENCL_1_2_APIS，clCreateCommandQueue 不会产生弃用警告。
target_compile_definitions(Image.Codec.OpenCL.Core PUBLIC
  CL_TARGET_OPENCL_VERSION=120)

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.OpenCL::Core)
add_library(Image.Codec.OpenCL::Core ALIAS Image.Codec.OpenCL.Core)
