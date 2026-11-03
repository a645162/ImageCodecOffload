# File: src/libs/CMakeLists.txt
# Brief: CMake build configuration.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# 本项目的一等库（与主项目 src/libs 同样的组织方式）：每个目录一个静态库，
# 提供 Image.Codec.Offload::<Name> 别名，使用方按需链接。
#
# 顶层按职责分三组，避免十几个库平铺在同一层：
#   codec/    通用 GPU 计算后端（OpenCL / CUDA / Vulkan），由开关决定是否加入
#   platform/ 厂商平台层（Intel oneVPL / NVIDIA nvJPEG·nvTIFF），同样由开关决定
#   offload/  路由层与 CPU 基线（Core / Common / Cpu / Router / Api），始终加入
#
# 顺序有意义：后端必须先于路由层加入，Router 才能在配置期用 if(TARGET ...)
# 判断某个后端是否真的编进了本次构建。
add_subdirectory(codec)
add_subdirectory(platform)
add_subdirectory(offload)
