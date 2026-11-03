# File: src/cli/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# 自测 CLI（icp_selftest）：验证硬件单元探测、硬件 JPEG 往返与 TIFF 容器。
#
# libjpeg / libtiff 是可选的：
#  - libjpeg 提供「软件 JPEG strip 编码器」，让 TIFF 容器在没有 Intel 媒体引擎
#    的机器上也能被完整验证；
#  - libtiff 只用于解码校验（它写 JPEG strip 会重新编码，所以不用来写文件）。
# 两者都没有时对应自测项明确 SKIP，而不是把整个程序变成不可构建。
add_executable(icp_selftest
  icp_selftest.cpp
  selftest_support.cpp)

target_link_libraries(icp_selftest PRIVATE
  Image.Codec.Platform.Intel::Jpeg::oneVPL
  Image.Codec.Platform.Intel::Tiff)

find_package(JPEG QUIET)
if(JPEG_FOUND)
  target_link_libraries(icp_selftest PRIVATE JPEG::JPEG)
  target_compile_definitions(icp_selftest PRIVATE OIC_HAVE_LIBJPEG)
  message(STATUS "icp_selftest: libjpeg ${JPEG_VERSION_STRING}（TIFF 容器可用软件编码器自测）")
else()
  message(STATUS "icp_selftest: 未找到 libjpeg（TIFF 容器自测将跳过）")
endif()

find_package(TIFF QUIET)
if(TIFF_FOUND)
  target_link_libraries(icp_selftest PRIVATE TIFF::TIFF)
  target_compile_definitions(icp_selftest PRIVATE OIC_HAVE_LIBTIFF)
  message(STATUS "icp_selftest: libtiff ${TIFF_VERSION_STRING}（用作容器解码校验）")
else()
  message(STATUS "icp_selftest: 未找到 libtiff（容器自测将跳过）")
endif()
