# File: src/libs/Image.Codec.Offload.Router/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

add_library(Image.Codec.Offload.Router STATIC
  src/router.h
  src/router.cpp
  src/opencl_backend.h
  src/opencl_backend.cpp
  src/cuda_backend.h
  src/cuda_backend.cpp
  src/vulkan_backend.h
  src/vulkan_backend.cpp)

target_include_directories(Image.Codec.Offload.Router PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(Image.Codec.Offload.Router PUBLIC
  Image.Codec.Offload.Core
  Image.Codec.Offload.Cpu)

# 适配层靠这两个宏决定「本构建到底有没有这个后端」：未定义时
# makeXxxBackend() 返回 nullptr，Router 仍然能编译并通过。
if(IMAGECODEC_OFFLOAD_WITH_OPENCL)
  target_compile_definitions(Image.Codec.Offload.Router PRIVATE
    IMAGECODEC_OFFLOAD_WITH_OPENCL=1)
  target_link_libraries(Image.Codec.Offload.Router PRIVATE
    Image.Codec.OpenCL.Api)
  if(IMAGECODEC_OFFLOAD_HAVE_OPENJPEG)
    target_compile_definitions(Image.Codec.Offload.Router PRIVATE
      IMAGECODEC_OFFLOAD_WITH_OPENJPEG=1)
  endif()
  if(IMAGECODEC_OFFLOAD_HAVE_JXL)
    target_compile_definitions(Image.Codec.Offload.Router PRIVATE
      IMAGECODEC_OFFLOAD_WITH_JXL=1)
  endif()
endif()

if(IMAGECODEC_OFFLOAD_WITH_CUDA)
  target_compile_definitions(Image.Codec.Offload.Router PRIVATE
    IMAGECODEC_OFFLOAD_WITH_CUDA=1)
  target_link_libraries(Image.Codec.Offload.Router PRIVATE
    Image.Codec.CUDA.Api)
  if(IMAGECODEC_OFFLOAD_HAVE_OPENJPEG)
    target_compile_definitions(Image.Codec.Offload.Router PRIVATE
      IMAGECODEC_OFFLOAD_WITH_OPENJPEG=1)
  endif()
  if(IMAGECODEC_OFFLOAD_HAVE_JXL)
    target_compile_definitions(Image.Codec.Offload.Router PRIVATE
      IMAGECODEC_OFFLOAD_WITH_JXL=1)
  endif()
endif()

if(IMAGECODEC_OFFLOAD_WITH_VULKAN)
  target_compile_definitions(Image.Codec.Offload.Router PRIVATE
    IMAGECODEC_OFFLOAD_WITH_VULKAN=1)
  target_link_libraries(Image.Codec.Offload.Router PRIVATE
    Image.Codec.Vulkan::CodecApi)
  if(IMAGECODEC_OFFLOAD_HAVE_OPENJPEG)
    target_compile_definitions(Image.Codec.Offload.Router PRIVATE
      IMAGECODEC_OFFLOAD_WITH_OPENJPEG=1)
  endif()
  if(IMAGECODEC_OFFLOAD_HAVE_JXL)
    target_compile_definitions(Image.Codec.Offload.Router PRIVATE
      IMAGECODEC_OFFLOAD_WITH_JXL=1)
  endif()
endif()

add_library(Image.Codec.Offload::Router ALIAS Image.Codec.Offload.Router)
