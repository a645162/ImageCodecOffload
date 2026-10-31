add_library(ImageCodecOffload.Router STATIC
  src/router.h
  src/router.cpp
  src/opencl_backend.h
  src/opencl_backend.cpp
  src/cuda_backend.h
  src/cuda_backend.cpp)

target_include_directories(ImageCodecOffload.Router PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(ImageCodecOffload.Router PUBLIC
  ImageCodecOffload.Core
  ImageCodecOffload.Cpu)

# 适配层靠这两个宏决定「本构建到底有没有这个后端」：未定义时
# makeXxxBackend() 返回 nullptr，Router 仍然能编译并通过。
if(IMAGECODEC_OFFLOAD_WITH_OPENCL)
  target_compile_definitions(ImageCodecOffload.Router PRIVATE
    IMAGECODEC_OFFLOAD_WITH_OPENCL=1)
  target_link_libraries(ImageCodecOffload.Router PRIVATE
    OpenCLImageCodec.Api)
endif()

if(IMAGECODEC_OFFLOAD_WITH_CUDA)
  target_compile_definitions(ImageCodecOffload.Router PRIVATE
    IMAGECODEC_OFFLOAD_WITH_CUDA=1)
  target_link_libraries(ImageCodecOffload.Router PRIVATE
    Cuda.Image.Codec.Api)
endif()

add_library(ImageCodecOffload::Router ALIAS ImageCodecOffload.Router)
