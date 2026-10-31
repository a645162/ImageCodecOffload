# src/ 下是本项目自己实现的代码：路由层、CPU 基线、C API、CLI。
# 各后端实现全部在 libs/（git submodule）里，本目录不碰它们。

add_subdirectory(libs)

if(IMAGECODEC_OFFLOAD_BUILD_CLI)
  add_subdirectory(cli)
endif()
