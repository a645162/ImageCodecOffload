# File: src/libs/Image.Codec.Vulkan.Jpeg/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# Image.Codec.Vulkan.Jpeg —— baseline JPEG 编解码（平台无关）。
#
# 容器与 Huffman 位流在 CPU（位流严格串行），DCT/IDCT、颜色变换、色度
# 上下采样在 Vulkan compute。分工与 CUDA / OpenCL 后端一致。
set(VIC_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
set(VIC_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${VIC_SPV_DIR}" "${VIC_GEN_DIR}")

set(VIC_SPV_FILES "")
foreach(s jpeg_dequant_idct jpeg_ycbcr_to_rgb jpeg_gray_to_y jpeg_rgb_to_ycbcr
          jpeg_downsample jpeg_fdct_quantize)
  vic_add_shader_spv("${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/${s}.comp"
                     "${VIC_SPV_DIR}/${s}.spv")
  list(APPEND VIC_SPV_FILES "${VIC_SPV_DIR}/${s}.spv")
endforeach()

set(VIC_SPV_HEADER "${VIC_GEN_DIR}/jpeg_spv_data.h")
vic_spv_to_header("${VIC_SPV_FILES}" "${VIC_SPV_HEADER}")

add_library(Image.Codec.Vulkan.Jpeg STATIC
  src/jpeg_bitstream.h
  src/jpeg_bitstream.cpp
  src/jpeg_huffman.h
  src/jpeg_huffman.cpp
  src/jpeg_dct_vk.h
  src/jpeg_dct_vk.cpp
  src/jpeg_dct_cpu.h
  src/jpeg_dct_cpu.cpp
  src/jpeg_codec.h
  src/jpeg_codec.cpp
  src/kernels/jpeg_dequant_idct.comp
  src/kernels/jpeg_ycbcr_to_rgb.comp
  src/kernels/jpeg_gray_to_y.comp
  src/kernels/jpeg_rgb_to_ycbcr.comp
  src/kernels/jpeg_downsample.comp
  src/kernels/jpeg_fdct_quantize.comp
  "${VIC_SPV_HEADER}"
  ${VIC_SPV_FILES})

target_include_directories(Image.Codec.Vulkan.Jpeg PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(Image.Codec.Vulkan.Jpeg PRIVATE
  "${VIC_GEN_DIR}")

target_link_libraries(Image.Codec.Vulkan.Jpeg PUBLIC
  Image.Codec.Vulkan::Common
  Vulkan::Vulkan)

add_library(Image.Codec.Vulkan::Jpeg ALIAS Image.Codec.Vulkan.Jpeg)

# CPU 参考必须与 GPU 逐位一致，所以禁止编译器把 a*b+c 收缩成 FMA：
# 着色器侧用 GLSL 的 precise 限制驱动，宿主侧就靠这条开关，两边都不能省。
# （与 CUDA 后端的 -fmad=false / -ffp-contract=off 是同一个契约。）
if(NOT MSVC)
  set_source_files_properties(src/jpeg_dct_cpu.cpp PROPERTIES
    COMPILE_OPTIONS "-ffp-contract=off")
endif()
