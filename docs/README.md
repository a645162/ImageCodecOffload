# ImageCodecOffload

图像编解码卸载仓库（git submodule，内部为嵌套 submodule），承载各计算后端（OpenCL / Vulkan）与平台/厂商专用库。

## 子项目

| 子项目 | 用途 | C API 前缀 | 依赖 |
|--------|------|-----------|------|
| `ImageCodecOpenCL` | OpenCL GPU 编解码：TIFF 容器 + LZW 编码(GPU)/解码(CPU)、baseline JPEG 混合编解码（Huffman CPU + DCT/颜色变换 GPU） | `image_codec_*`（`<OpenCLImageCodec/image_codec.h>`） | 仅 OpenCL（vcpkg `opencl`） |
| `ImageCodecPlatform` | 平台抽象（exe_dir/单调时钟/休眠）与 Intel oneVPL（`libvpl.dll` 动态加载）JPEG 硬件后端；未来承载 nvJPEG/nvTIFF 等厂商专用后端 | `image_codec_*` | `libvpl.dll` + D3D11（Windows） |
| `ImageCodecVulkan` | Vulkan 后端（预留） | - | Vulkan SDK |

## 约束

- `ImageCodecOpenCL` **不引用** `ImageCodecPlatform`（纯 OpenCL，自包含）。
- `ImageCodecPlatform` 仅承载平台抽象与厂商/Intel 专用后端，供上层按需接入。

## 目录结构

各子项目结构与主项目一致：

```
src/
├── CMakeLists.txt
└── libs/
    ├── OpenCL.Image.Codec.<Module>/   # 独立静态库，可单独 target_link_libraries
    │   └── src/                       # 源码
    └── CMakeLists.txt                 # 库清单
```

## 文档

- `docs/plan/`：实施计划
- `docs/report/`：研究报告
