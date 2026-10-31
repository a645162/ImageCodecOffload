cmake_minimum_required(VERSION 3.24)

# nvcc 不一定在 PATH 上（例如只通过 /usr/local/cuda-13.2 安装）。若下面打开了
# IMAGECODEC_OFFLOAD_WITH_CUDA，需要在 project() 之前把编译器找出来，否则
# enable_language(CUDA) 会直接失败。
option(IMAGECODEC_OFFLOAD_WITH_CUDA "Build the CUDA backend (ImageCodecCUDA)" OFF)

if(IMAGECODEC_OFFLOAD_WITH_CUDA AND NOT DEFINED CMAKE_CUDA_COMPILER AND NOT DEFINED ENV{CUDACXX})
  find_program(_ico_nvcc
    NAMES nvcc
    HINTS "${CUDAToolkit_ROOT}/bin" /usr/local/cuda/bin
    PATHS "$ENV{CUDA_HOME}/bin" "$ENV{CUDA_PATH}/bin" /usr/local/cuda-13.2/bin
    DOC "CUDA compiler")
  if(_ico_nvcc)
    set(CMAKE_CUDA_COMPILER "${_ico_nvcc}" CACHE FILEPATH "CUDA compiler")
    get_filename_component(_ico_cuda_root "${_ico_nvcc}" DIRECTORY)
    get_filename_component(_ico_cuda_root "${_ico_cuda_root}" DIRECTORY)
    if(NOT DEFINED CUDAToolkit_ROOT AND EXISTS "${_ico_cuda_root}/include/cuda_runtime.h")
      set(CUDAToolkit_ROOT "${_ico_cuda_root}" CACHE PATH "CUDA Toolkit root")
    endif()
  endif()
endif()

project(ImageCodecOffload LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码为 UTF-8（含中文注释）；MSVC 默认按系统代码页解析会乱码并破坏词法。
if(MSVC)
  add_compile_options(/utf-8)
endif()

# ---- 后端开关 ----
# 默认只开 OpenCL：它在三家里覆盖面最广（Intel/NVIDIA/AMD），并且不要求
# 额外的工具链。CUDA / Vulkan 由使用方按机器显式打开。
option(IMAGECODEC_OFFLOAD_WITH_OPENCL "Build the OpenCL backend (ImageCodecOpenCL)" ON)
option(IMAGECODEC_OFFLOAD_WITH_VULKAN "Build the Vulkan backend (ImageCodecVulkan)" OFF)
option(IMAGECODEC_OFFLOAD_BUILD_CLI "Build the ico CLI" ON)
option(IMAGECODEC_OFFLOAD_BUILD_TESTS "Build the test suite" OFF)

# ---- CPU 基线依赖 ----
# 三个格式各有自己的库：libtiff 处理容器与 LZW，libpng 处理 PNG，
# libjpeg-turbo 处理 baseline JPEG。它们同时是 GPU 后端的像素级参照。
find_package(TIFF REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)

if(IMAGECODEC_OFFLOAD_WITH_CUDA)
  # 子项目的 CMakeLists 也会 project(... LANGUAGES ... CUDA)，这里先启用一次，
  # 让编译器探测失败的报错出现在「本项目的配置阶段」而不是某个子目录里。
  enable_language(CUDA)
endif()

# ---- 子项目（git submodule）----
if(IMAGECODEC_OFFLOAD_WITH_OPENCL)
  add_subdirectory(libs/ImageCodecOpenCL)
endif()

if(IMAGECODEC_OFFLOAD_WITH_CUDA)
  add_subdirectory(libs/ImageCodecCUDA)
endif()

if(IMAGECODEC_OFFLOAD_WITH_VULKAN)
  # 现状：ImageCodecVulkan 只有 Vulkan compute 与 Win32 external-memory -> GL
  # 互操作，没有任何 TIFF/PNG/JPEG 编解码与指标实现。打开它只会得到一个
  # 「注册了但什么都做不了」的后端，因此这里明确拒绝，而不是静默跳过。
  message(FATAL_ERROR
    "IMAGECODEC_OFFLOAD_WITH_VULKAN 暂不可用：libs/ImageCodecVulkan 目前还没有"
    " TIFF/PNG/JPEG 编解码与画质指标实现。请先用 OpenCL 或 CUDA 后端。")
endif()

# ---- 本项目的路由层 / API / CLI ----
add_subdirectory(src)

if(IMAGECODEC_OFFLOAD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
