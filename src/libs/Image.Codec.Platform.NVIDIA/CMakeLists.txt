# File: src/libs/Image.Codec.Platform.NVIDIA/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

# Image.Codec.Platform.NVIDIA —— NVIDIA 厂商编解码库的厂商层工程。
#
# 与 Image.Codec.Platform.Intel 的分工完全对称：那边承载 Intel 媒体引擎
# （oneVPL，运行期动态加载、不需要导入库），这边承载 NVIDIA 的三件事——
# nvJPEG（baseline JPEG）、nvJPEG2000（JPEG 2000 / JP2）、nvTIFF（TIFF）。
#
# 本工程刻意**不依赖 Image.Codec.CUDA**：它只需要 CUDA Runtime（cudart）与
# 三个厂商运行库，不需要 nvcc 编译自己的任何源码。因此没有 CUDA 显卡的
# 机器也能配置和编译（运行期由 available() 如实报告不可用），而 Image.Codec.CUDA
# 那套 nvcc / fatbin / 架构选择与本工程无关。
cmake_minimum_required(VERSION 3.16)
project(Image.Codec.Platform.NVIDIA LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8)
endif()

# 自测 CLI 默认不构建（它不是库的一部分，只用于本机验证三个厂商库与往返）。
option(OIC_BUILD_CLI "Build test CLI" OFF)
# nvJPEG 随 CUDA Toolkit 提供；nvJPEG2000 / nvTIFF 是独立安装的包。三者都允许
# 缺失：缺哪个就把对应的 available() 置假并给出原因，绝不悄悄换实现。
option(OIC_NVIDIA_WITH_JPEG "Build the nvJPEG codec (Image.Codec.Platform.NVIDIA.Jpeg)" ON)
option(OIC_NVIDIA_WITH_JPEG2K "Build the nvJPEG2000 codec (Image.Codec.Platform.NVIDIA.Jpeg2K)" ON)
option(OIC_NVIDIA_WITH_TIFF "Build the nvTIFF codec (Image.Codec.Platform.NVIDIA.Tiff)" ON)

# CUDA Runtime 只提供头与 cudart（不需要 enable_language(CUDA)）。作为主项目
# 的子目录复用时，CUDA::cudart 往往已经存在，这里不重复 find_package。
if(NOT TARGET CUDA::cudart)
  find_package(CUDAToolkit REQUIRED)
endif()

# nvJPEG 在 CUDA Toolkit 里，是 CUDA::nvjpeg 导入目标。
if(OIC_NVIDIA_WITH_JPEG AND NOT TARGET CUDA::nvjpeg)
  message(STATUS
    "Image.Codec.Platform.NVIDIA: 未找到 CUDA::nvjpeg，JPEG 编解码将报告 UNSUPPORTED")
  set(OIC_NVIDIA_WITH_JPEG OFF)
endif()

# nvJPEG2000 / nvTIFF 是独立安装的包，各自带版本目录（libnvjpeg2k/<major>、
# libnvtiff0-dev-cuda-<major>）。这里按「头 + 库」成对探测，不做版本写死；
# 版本目录由 /usr/include 上的 alternatives 符号链接或包自身的头目录承担。
function(oic_nvidia_find_component prefix header library)
  find_path(${prefix}_INCLUDE_DIR NAMES ${header})
  find_library(${prefix}_LIBRARY NAMES ${library})
  set(${prefix}_FOUND FALSE)
  if(${prefix}_INCLUDE_DIR AND ${prefix}_LIBRARY)
    set(${prefix}_FOUND TRUE)
    if(NOT TARGET NVIDIA::${library})
      add_library(NVIDIA::${library} UNKNOWN IMPORTED)
      set_target_properties(NVIDIA::${library} PROPERTIES
        IMPORTED_LOCATION "${${prefix}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${${prefix}_INCLUDE_DIR}")
    endif()
  endif()
  set(${prefix}_FOUND ${${prefix}_FOUND} PARENT_SCOPE)
endfunction()

if(OIC_NVIDIA_WITH_JPEG2K)
  oic_nvidia_find_component(NVJPEG2K nvjpeg2k.h nvjpeg2k)
  if(NOT NVJPEG2K_FOUND)
    message(STATUS
      "Image.Codec.Platform.NVIDIA: 未找到 nvJPEG2000（头 nvjpeg2k.h / 库 nvjpeg2k），JPEG 2000 将报告 UNSUPPORTED")
    set(OIC_NVIDIA_WITH_JPEG2K OFF)
  endif()
endif()

if(OIC_NVIDIA_WITH_TIFF)
  oic_nvidia_find_component(NVTIFF nvtiff.h nvtiff)
  if(NOT NVTIFF_FOUND)
    message(STATUS
      "Image.Codec.Platform.NVIDIA: 未找到 nvTIFF（头 nvtiff.h / 库 nvtiff），TIFF 将报告 UNSUPPORTED")
    set(OIC_NVIDIA_WITH_TIFF OFF)
  endif()
endif()

add_subdirectory(src)
