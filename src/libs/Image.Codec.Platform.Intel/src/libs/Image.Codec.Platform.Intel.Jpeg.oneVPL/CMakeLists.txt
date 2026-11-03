# File: src/libs/Image.Codec.Platform.Intel.Jpeg.oneVPL/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-08-12

# oneVPL（Intel 媒体引擎）JPEG 后端：Linux + Windows 双平台。
#
# 可移植性说明：
#  - oneVPL API 头经 git submodule 引入（仓库根 3rdparty/libvpl，
#    https://github.com/intel/libvpl），头文件位于其 api/vpl/，未做任何复制。
#    Windows 用 LoadLibraryA("libvpl.dll")，Linux 用 dlopen("libvpl.so.2")，
#    都不需要导入库。
#  - 硬件实现由运行库在运行期自行加载：Intel 核显需要 vpl-gpu-rt
#    （Windows: libmfx-gen.dll，Linux: libmfx-gen.so.1）。缺它时枚举为空、
#    vpl_probe() 会明确报告，而不是让程序在 Init 处报一个看不懂的错误。
#  - 帧只走系统内存（MFX_IOPATTERN_*_SYSTEM_MEMORY），不再依赖 D3D11 / DXGI，
#    因此 Linux（VA-API）与 Windows 共用同一份编解码代码。
#  - 首次克隆需 `git submodule update --init --recursive`。
add_library(Image.Codec.Platform.Intel.Jpeg.oneVPL STATIC
  src/bmp24.cpp
  src/onevpl_test.cpp
  src/rgb_nv12.cpp
  src/vpl_device.cpp
  src/vpl_dyn.cpp
  src/vpl_jpeg_decoder.cpp
  src/vpl_jpeg_encoder.cpp)

target_include_directories(Image.Codec.Platform.Intel.Jpeg.oneVPL PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${PROJECT_SOURCE_DIR}/3rdparty/libvpl/api/vpl")

target_link_libraries(Image.Codec.Platform.Intel.Jpeg.oneVPL PUBLIC
  Image.Codec.Platform.Intel.Core)

if(WIN32)
  # mfx 头与 windows.h 共存时避免 min/max 宏冲突。
  target_compile_definitions(Image.Codec.Platform.Intel.Jpeg.oneVPL PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
else()
  # dlopen/dlsym 在 glibc 2.34+ 已并入 libc，老系统仍需 libdl。
  target_link_libraries(Image.Codec.Platform.Intel.Jpeg.oneVPL PRIVATE ${CMAKE_DL_LIBS})
endif()

if(NOT EXISTS "${PROJECT_SOURCE_DIR}/3rdparty/libvpl/api/vpl/mfxvideo.h")
  message(FATAL_ERROR
    "缺少 oneVPL 头文件：${PROJECT_SOURCE_DIR}/3rdparty/libvpl/api/vpl。\n"
    "请先执行 git submodule update --init --recursive")
endif()

# 命名空间别名：target_link_libraries(app PRIVATE Image.Codec.Platform.Intel::Jpeg::oneVPL)
add_library(Image.Codec.Platform.Intel::Jpeg::oneVPL ALIAS Image.Codec.Platform.Intel.Jpeg.oneVPL)
