# File: src/libs/Image.Codec.Platform.NVIDIA/src/cli/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

# 自测 CLI（inp_selftest）：设备枚举 + 三个厂商库的编解码往返。
#
# 缺某个厂商库时对应项在程序内明确 SKIP——这是「本机没有那个包」的正常状态，
# 不是构建失败。
add_executable(inp_selftest inp_selftest.cpp)

target_link_libraries(inp_selftest PRIVATE
  Image.Codec.Platform.NVIDIA::Core)

if(TARGET Image.Codec.Platform.NVIDIA.Jpeg)
  target_link_libraries(inp_selftest PRIVATE Image.Codec.Platform.NVIDIA::Jpeg)
  target_compile_definitions(inp_selftest PRIVATE OIC_NVIDIA_WITH_JPEG=1)
endif()
if(TARGET Image.Codec.Platform.NVIDIA.Jpeg2K)
  target_link_libraries(inp_selftest PRIVATE Image.Codec.Platform.NVIDIA::Jpeg2K)
  target_compile_definitions(inp_selftest PRIVATE OIC_NVIDIA_WITH_JPEG2K=1)
endif()
if(TARGET Image.Codec.Platform.NVIDIA.Tiff)
  target_link_libraries(inp_selftest PRIVATE Image.Codec.Platform.NVIDIA::Tiff)
  target_compile_definitions(inp_selftest PRIVATE OIC_NVIDIA_WITH_TIFF=1)
endif()
