# ImageCodecOffload

图像编解码卸载仓库（git submodule，内部为嵌套 submodule）。它既是各 GPU 后端的集合，
也提供**主项目自己的路由层**：调用方说「压一张图」，由它挑后端、在失败时回退、并说清楚
每一次回退的原因。

```
调用方（ImageViewer / 脚本 / 自己的程序）
   │  C API: image_codec_offload_*（include/ImageCodecOffload/image_codec_offload.h）
   ▼
src/libs/ImageCodecOffload.Router   ← 后端选择 / 回退 / 记录原因
   ├─ ImageCodecOffload.Cpu         ← libtiff / libpng / libjpeg-turbo（永远可用，也是像素级参照）
   ├─ ImageCodecOffload.Router ──►  libs/ImageCodecCUDA   （nvJPEG / GPU LZW / GPU 行滤波）
   └─ ImageCodecOffload.Router ──►  libs/ImageCodecOpenCL （混合 JPEG / GPU LZW）
        ▲
        └─ ImageCodecOffload.Core / .Common / .Api（类型、指标 CPU 参考、BMP 中转、C API）
```

## 子项目

| 子项目 | 用途 | C API 前缀 | 依赖 |
|--------|------|-----------|------|
| `ImageCodecCUDA` | NVIDIA 后端：TIFF LZW 编码(GPU)、PNG 行滤波(GPU)、baseline JPEG 全 nvJPEG，以及 GPU 画质指标与调参库 | `image_codec_cuda_*`（`<ImageCodecCUDA/image_codec_cuda.h>`） | CUDA Toolkit（nvJPEG）+ vcpkg `zlib` |
| `ImageCodecOpenCL` | OpenCL GPU 编解码：TIFF 容器 + LZW 编码(GPU)/解码(CPU)、baseline JPEG 混合编解码（Huffman CPU + DCT/颜色变换 GPU） | `image_codec_*`（`<OpenCLImageCodec/image_codec.h>`） | 仅 OpenCL（vcpkg `opencl`） |
| `ImageCodecPlatform` | 平台抽象（exe_dir/单调时钟/休眠）与 Intel oneVPL（`libvpl.dll` 动态加载）JPEG 硬件后端 | `image_codec_*` | `libvpl.dll` + D3D11（Windows） |
| `ImageCodecVulkan` | Vulkan compute 与 Win32 external-memory → OpenGL 互操作 | - | Vulkan SDK / vcpkg `vulkan` |

## 能力矩阵（现状）

| 格式 | CUDA | OpenCL | CPU |
| --- | --- | --- | --- |
| JPEG | 编码/解码（nvJPEG，1 或 3 通道） | 编码/解码（**仅 3 通道**） | 编码/解码（1 或 3 通道） |
| TIFF | 编码/解码（LZW/none，strip 与 tiled，1/3/4 通道） | 编码/解码（**仅 3 通道**） | 编码/解码 |
| PNG | 编码/解码（1/3/4 通道） | **不支持** | 编码/解码 |
| MSE/PSNR/SSIM/MS-SSIM | 有 | 有 | 有（参考实现） |

Vulkan 目前**没有任何 TIFF/PNG/JPEG 编解码与指标实现**，因此
`IMAGECODEC_OFFLOAD_WITH_VULKAN=ON` 在 configure 阶段直接 `FATAL_ERROR`，
而不是注册一个「存在但什么都做不了」的后端。

## 回退语义

这是本项目最容易被改坏的地方，规则写死在这里：

| 失败类别 | 判定 | 行为 |
| --- | --- | --- |
| 后端不可用 / 不支持该格式或方向 / 资源不足 | 这个后端**不适合这一单** | 记进 notes，继续试下一个 |
| 解码失败 / 编码失败 / IO / 内部错误 | 这个后端**做砸了** | 也记进 notes、**不拦路**（1.9 GB 的图不该因为 OpenCL 分配不出设备缓冲就连 CPU 都不试），但如果所有后端都失败，返回第一个这样的错误 |
| 参数非法 | 调用方的 bug | 立刻中止，不换后端、不掩盖 |

强制入口（`ico encode --backend opencl`、`Router::encodeWith`）**永不回退**：想看某个后端
的真实表现时，回退只会让你看到另一个后端的结果。

`Outcome::notes` 里是每一次回退的可读原因，`ico` 会原样打印：

```
32000x19656 x3 -> jpeg: 120437390 bytes in 25962.7 ms (69.3 MiB/s)
backend: cpu  (fell back)
  - cuda: out-of-memory (cuda: encode needs more device memory than available)
  - opencl: encode-failed (opencl: encode failed: encode failed)
```

## 通道保真（硬性约束）

**1 通道进必须 1 通道出。** 灰度图被悄悄展开成 3 通道时，体积、压缩比与上层「这张图是不是
灰度」的判断会一起错位，而肉眼几乎看不出来。因此：

- CPU 基线全程保留 1 通道；
- CUDA 灰度 JPEG 走 `CSS_GRAY`（真 1 分量文件），灰度 TIFF/PNG 也保持 1 通道；
- OpenCL 的 C ABI 只有 24-bit RGB 一种形态，于是**调用之前**先用
  `ImageCodecOffload.Cpu` 的容器头探测（`probeChannels`）问出「这张图本来几通道」，
  表示不了就明确返回 `unsupported-format`，交给 CPU。允许「做不到」，不允许
  「做到了但改变了语义」。

## 构建

依赖 libtiff / libpng / libjpeg-turbo（CPU 基线），OpenCL 与 CUDA 按需打开。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DIMAGECODEC_OFFLOAD_BUILD_TESTS=ON \
      -DIMAGECODEC_OFFLOAD_WITH_CUDA=ON     # 可选
cmake --build build -j
```

选项：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `IMAGECODEC_OFFLOAD_WITH_OPENCL` | `ON` | 构建 OpenCL 后端 |
| `IMAGECODEC_OFFLOAD_WITH_CUDA` | `OFF` | 构建 CUDA 后端（顶层会在 `project()` 前探测 `nvcc`） |
| `IMAGECODEC_OFFLOAD_WITH_VULKAN` | `OFF` | 打开即 `FATAL_ERROR`（见上） |
| `IMAGECODEC_OFFLOAD_BUILD_CLI` | `ON` | 构建 `ico` |
| `IMAGECODEC_OFFLOAD_BUILD_TESTS` | `OFF` | 构建并注册 ctest（`ico_test` + 各子项目测试） |

## CLI `ico`

```
ico backends                                    # 每个后端当前是否 ready，以及原因
ico info <file>                                 # 格式 / 尺寸 / 通道数（不解像素）
ico encode <in> <out> [--quality N] [--compression N] [--tile N]
                      [--rows N] [--png-level N] [--png-filter N]
                      [--backend cpu|opencl|cuda]
ico decode <in> <out.bmp> [--backend cpu|opencl|cuda]
ico compare <a> <b> [--backend cpu|opencl|cuda]  # 三后端的 MSE/PSNR/SSIM/MS-SSIM 并排
```

BMP 只作为中转格式（`info`/`decode` 的输出、`compare` 的输入），它不是目标格式。

## 约束

- `ImageCodecOpenCL` **不引用** `ImageCodecPlatform`（纯 OpenCL，自包含）。
- `ImageCodecPlatform` 仅承载平台抽象与厂商/Intel 专用后端，供上层按需接入。
- `ImageCodecVulkan` 可通过 `add_subdirectory(... EXCLUDE_FROM_ALL)` 被上层复用；
  使用 `VulkanImageCodec::Api`，不要复制其 Win32 handle/GL 导入实现。
- 三个后端的画质指标必须逐项给出**同一个数字**（golden 常量锁在
  `tests/ico_test.cpp`）。换后端不换数字，改公式就是改契约。
- 子项目的 CMake 用 `PROJECT_SOURCE_DIR` 定位自己的 `include/`：
  它们会被 `add_subdirectory` 复用，`CMAKE_SOURCE_DIR` 会指向父工程根目录。

## 目录结构

```
CMakeLists.txt                     # 后端开关、CPU 基线依赖、子项目挂载
include/ImageCodecOffload/         # 对外 C API（ico_*）
src/
├── cli/                           # ico
└── libs/
    ├── ImageCodecOffload.Core/     # 类型 / 错误 / 指标 CPU 参考 / ICodecBackend
    ├── ImageCodecOffload.Common/   # BMP 读写 + probeBmp
    ├── ImageCodecOffload.Cpu/      # libtiff / libpng / libjpeg-turbo + 容器通道探测
    ├── ImageCodecOffload.Router/   # OpenCL / CUDA 适配 + 回退
    └── ImageCodecOffload.Api/      # C API 实现
libs/                              # 子项目（git submodule）
tests/                             # ico_test
docs/                              # plan / report / 本文件
```

## 三级提交约定

1. **子项目**（`ImageCodecCUDA` / `ImageCodecOpenCL` / `ImageCodecPlatform` / `ImageCodecVulkan`）：
   先在各自仓库内提交代码改动（含其内部 `docs/`）。
2. **父仓库** `ImageCodecOffload`：`git add libs/<子项目> docs` 后提交——更新子项目指针；
   同步更新本文件与 `docs/plan/`、`docs/report/`。
3. **主项目** `ImageViewerQt`：`git add libs/ImageCodecOffload` 提交，更新父仓库指针；
   并同步更新 `AGENTS.md` 的 ImageCodecOffload 说明与 `docs/.docs/libs/` 下的 About 文档。

## 文档

- `docs/plan/`：实施计划
- `docs/report/`：研究报告与实测数据
- `libs/ImageCodecCUDA/docs/`：CUDA 后端的架构、指标定义与调参说明
