# File: src/libs/Image.Codec.Platform.NVIDIA/src/libs/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-11-03

# NVIDIA 厂商库（每个编解码库一个目标，可按需链接）。
add_subdirectory(Image.Codec.Platform.NVIDIA.Core)    # 设备 / 流 / 显存记账 / 错误文本
if(OIC_NVIDIA_WITH_JPEG)
  add_subdirectory(Image.Codec.Platform.NVIDIA.Jpeg)   # nvJPEG：baseline JPEG
endif()
if(OIC_NVIDIA_WITH_JPEG2K)
  add_subdirectory(Image.Codec.Platform.NVIDIA.Jpeg2K) # nvJPEG2000：JPEG 2000 / JP2
endif()
if(OIC_NVIDIA_WITH_TIFF)
  add_subdirectory(Image.Codec.Platform.NVIDIA.Tiff)   # nvTIFF：TIFF
endif()

# 聚合入口：target_link_libraries(app PRIVATE Image.Codec.Platform.NVIDIA::All)
# 只聚合本次构建真实存在的组件——缺 nvTIFF 的机器上不该出现一个空目标的链接错误。
add_library(Image.Codec.Platform.NVIDIA.All INTERFACE)
add_library(Image.Codec.Platform.NVIDIA::All ALIAS Image.Codec.Platform.NVIDIA.All)
target_link_libraries(Image.Codec.Platform.NVIDIA.All INTERFACE
  Image.Codec.Platform.NVIDIA::Core)
if(TARGET Image.Codec.Platform.NVIDIA.Jpeg)
  target_link_libraries(Image.Codec.Platform.NVIDIA.All INTERFACE
    Image.Codec.Platform.NVIDIA::Jpeg)
endif()
if(TARGET Image.Codec.Platform.NVIDIA.Jpeg2K)
  target_link_libraries(Image.Codec.Platform.NVIDIA.All INTERFACE
    Image.Codec.Platform.NVIDIA::Jpeg2K)
endif()
if(TARGET Image.Codec.Platform.NVIDIA.Tiff)
  target_link_libraries(Image.Codec.Platform.NVIDIA.All INTERFACE
    Image.Codec.Platform.NVIDIA::Tiff)
endif()
