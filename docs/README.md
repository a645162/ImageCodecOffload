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
   ├─ ImageCodecOffload.Router ──►  libs/ImageCodecOpenCL （混合 JPEG / GPU LZW）
   └─ ImageCodecOffload.Router ──►  libs/ImageCodecVulkan （混合 JPEG / TIFF 预测器 / PNG 行滤波 / 指标）
        ▲
        └─ ImageCodecOffload.Core / .Common / .Api（类型、指标 CPU 参考、BMP 中转、C API）
```

## 子项目

| 子项目 | 用途 | C API 前缀 | 依赖 |
|--------|------|-----------|------|
| `ImageCodecCUDA` | NVIDIA 后端：TIFF LZW 编码(GPU)、PNG 行滤波(GPU)、baseline JPEG 全 nvJPEG，以及 GPU 画质指标与调参库 | `image_codec_cuda_*`（`<ImageCodecCUDA/image_codec_cuda.h>`） | CUDA Toolkit（nvJPEG）+ vcpkg `zlib` |
| `ImageCodecOpenCL` | OpenCL GPU 编解码：TIFF 容器 + LZW 编码(GPU)/解码(CPU)、PNG 行滤波(GPU) + zlib、baseline JPEG 混合编解码（Huffman CPU + DCT/颜色变换 GPU） | `image_codec_*`（`<OpenCLImageCodec/image_codec.h>`） | 仅 OpenCL（vcpkg `opencl`） |
| `ImageCodecPlatform` | 平台抽象（exe_dir / 单调时钟 / 休眠）+ Intel 媒体引擎（oneVPL，`libvpl.dll` / `libvpl.so.2` 运行期动态加载）：硬件 baseline JPEG 编解码、硬件单元探测，以及把硬件 JPEG 写成 TIFF（Compression=7 JPEG strips） | -（尚未接入 `ico` / 路由表） | 构建期仅 oneVPL 头（`3rdparty/libvpl`）；运行期 `libvpl2` + `libmfx-gen1.2`（vpl-gpu-rt）+ `intel-media-va-driver`（Windows：Intel 显卡驱动自带 `libvpl.dll`/`libmfx-gen.dll`）。Linux / Windows 双平台，帧只走系统内存 |
| `ImageCodecVulkan` | Vulkan compute 编解码：TIFF（容器 + LZW(CPU) + Predictor=2(GPU)）、PNG（行滤波 GPU + DEFLATE(CPU)）、baseline JPEG（Huffman/容器 CPU + DCT/颜色变换 GPU）、画质指标（fp64）；另含 Win32 external-memory → OpenGL 互操作（仅 Windows） | `image_codec_vulkan_*`（`<VulkanImageCodec/image_codec_vulkan.h>`） | Vulkan SDK / vcpkg `vulkan`；GLSL 编译器（`glslc` 或 `glslangValidator`，只在构建期用） |

## 能力矩阵（现状）

| 格式 | CUDA | OpenCL | Vulkan | CPU |
| --- | --- | --- | --- | --- |
| JPEG | 编码/解码（nvJPEG，1 或 3 通道；2/4 明确拒绝） | 编码/解码（1 或 3 通道，子采样可指定；2/4 明确拒绝） | 编码/解码（1 或 3 通道，子采样可指定；2/4 明确拒绝） | 编码/解码（1 或 3 通道；2/4 明确拒绝） |
| TIFF | 编码/解码（LZW/none，strip 与 tiled，1/2/3/4 通道） | 编码/解码（none/LZW，strip 与 tiled，1/2/3/4 通道） | 编码/解码（none/LZW，strip 与 tiled，1/2/3/4 通道；Predictor=2 在 GPU，LZW 在 CPU） | 编码/解码（1/2/3/4 通道） |
| PNG | 编码/解码（1/2/3/4 通道） | 编码/解码（行滤波 GPU + DEFLATE CPU，1/2/3/4 通道） | 编码/解码（行滤波 GPU + DEFLATE CPU，1/2/3/4 通道） | 编码/解码（1/2/3/4 通道） |
| MSE/PSNR/SSIM/MS-SSIM | 有 | 有 | 有（要求 `shaderFloat64`，申请不到即明确失败） | 有（参考实现） |

Vulkan 后端的**分工刻意与 GPU 并行度对齐**：JPEG 的 DCT/IDCT、颜色变换、色度上下采样、
PNG 行滤波与 TIFF Predictor=2 都是逐像素/逐行独立的工作，放在 GPU；JPEG 的 Huffman 位流、
LZW 字典构建与 zlib deflate/inflate 严格串行，留在 CPU。`capabilities.tiff_lzw_gpu` 因此
如实报 0，而不是给一个「看着有 GPU 参与、其实更慢」的路径。

**GPU 与 CPU 参考逐位一致**（Vulkan 后端自带一份 CPU DCT，只在 `use_gpu=false` 与自测里用）：
颜色变换用 libjpeg 的 16 位定点系数（浮点 BT.601 会被驱动的 FMA 合并，1 个 ulp 的差异经
`(a+b+c+d+2)>>2` 会放大成 ±1 的色度偏差），DCT 累加用 GLSL `precise` + `-ffp-contract=off`
禁止收缩，量化舍入带 1e-3 的平局偏置——否则「精确落在 n+0.5」的系数（DC 很常见）会让两份
实现编出不同的量化系数。`tests/codec_api_test.cpp` 对两者做字节级比对。

**色度下采样的抽取比例必须由采样因子给出**（`h_ratio = max_h / h_samp`、
`v_ratio = max_v / v_samp`）：4:2:0 是 2x2，4:2:2 是 **2x1**（色度平面与亮度等高）。
两个子采样共用一个下采样实现时很容易把 2x2 写死，那样 4:2:2 的 `sy = y*2` 会越过平面底部
被 clamp 成同一行——文件能打开、亮度正常，只有下半幅颜色被摊平（平滑渐变图从 33.6 dB 掉到
18.9 dB）。OpenCL 后端曾同时踩到另一个同类坑：把 `max_v` 留在 4:2:0 的 2，`mcus_y` 按 16 行
算而 SOF 里写 `v_samp=1`，解码器整幅色度错位。`tests/ico_test.cpp` 的 `testJpegSubsampling`
用平滑渐变图把这两条都钉住。

## 回退语义

这是本项目最容易被改坏的地方，规则写死在这里：

| 失败类别 | 判定 | 行为 |
| --- | --- | --- |
| 后端不可用 / 不支持该格式或方向 / **这组参数该后端表达不了** / 资源不足 | 这个后端**不适合这一单** | 记进 notes，继续试下一个 |
| 解码失败 / 编码失败 / IO / 内部错误 | 这个后端**做砸了** | 也记进 notes、**不拦路**（1.9 GB 的图不该因为 OpenCL 分配不出设备缓冲就连 CPU 都不试），但如果所有后端都失败，返回第一个这样的错误 |
| 参数非法 | 调用方的 bug | 立刻中止，不换后端、不掩盖 |

强制入口（`ico encode --backend opencl`、`Router::encodeWith`）**永不回退**：想看某个后端
的真实表现时，回退只会让你看到另一个后端的结果。

C API 上对应两个入口，**不要混用**：

- `image_codec_offload_create(codec, preferred)` / `set_order(...)`：**优先**，永远保留 CPU 兜底。
  适合「压一张图，谁快谁上」的单次调用。
- `image_codec_offload_create_forced(codec, backend)`：**强制**，失败即失败。
  适合「一条 lane = 一个后端」的并发调度（存图管线的 CPU lane 与 GPU lane 必须真的不同，
  否则两条 lane 会在同一块设备上互抢显存，统计也不再可解释）。

`set_order` 无法表达「只用 CPU」是刻意的：兜底不能丢。所以需要纯 CPU 时只能用 forced 入口。

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
- OpenCL 的 BMP 中转层（`OpenCL.Image.Codec.Common`）本身区分 8-bit 灰度与
  24-bit RGB：灰度 TIFF 写 `spp=1` + `Photometric=BlackIsZero`，灰度 JPEG 走
  `gray_to_y` 并强制 4:4:4（没有色度可下采样），灰度 PNG 按 1 通道行滤波；
- 路由层仍然先用 `ImageCodecOffload.Cpu` 的容器头探测（`probeChannels`）问出
  「这张图本来几通道」，与后端能表示的通道数不匹配就明确返回
  `unsupported-format`，交给下一个后端。允许「做不到」，不允许「做到了但改变了语义」。

## 编码参数与能力表

`ico_encode_params` 覆盖到「与源工具的存图参数一一对应」为止，缺一项就意味着上层只能在
自己没有参数时才能保证字节一致：

| 格式 | 参数 |
| --- | --- |
| JPEG | `quality` / `subsampling` / `progressive` / `optimize_huffman` / `restart_interval` / `luma_quality` / `chroma_quality` |
| TIFF | `compression`（1 none / 5 LZW / 7 JPEG / 8 AdobeDeflate / 32773 PackBits）/ `rows_per_strip` / `tile_size` / `predictor` / `x_dpi` / `y_dpi` / `resolution_unit` / `jpeg_quality` |
| PNG | `compression` / `filter` / `strategy`（0 default / 1 filtered / 2 huffman / 3 rle / 4 fixed） |

**「0 / 负数 = 未指定」，PNG 的两个字段除外**：C 调用方习惯把结构体清零，若把 0 当成
「质量 0」就会被夹到 1，而调用方以为自己什么都没改。所以除 PNG 外，清零后的结构体必须与
内部默认值产出**逐字节相同**的文件（`tests/ico_test.cpp` 的 `testCapiAdvancedParams` 对
JPEG/TIFF 做了这条比对）。

PNG 的 `png_compression` 与 `png_filter` 是刻意的不对称：zlib 等级 0 与「不滤波」都是合法
取值，用 0 表示未指定会让它们永远传不进去（这两个值确实曾经丢失过），所以它们的「未指定」
是 **-1**，清零结构体等于「等级 0 + filter None」。

**TIFF 默认不写 `Predictor` 标签**（`tiff_predictor <= 0`）。值为 1 的 Predictor 也是合法的
「无预测」，但会多一个 12 字节的 IFD 条目——于是同一张图、同一组参数，走 libtiff 的旧写法
与走本库就差 12 字节，「逐字节一致」永远不成立。要显式声明 `PREDICTOR_NONE` 就传 1
（测试里锁死了「比默认多恰好 12 字节」）。

**能力表 `EncodeCapabilities` / `ico_encode_capabilities`** 回答的是「这个后端会不会把我的
参数悄悄丢掉」，与「值域合不合法」无关：

- 每个后端实现 `ICodecBackend::encodeCapabilities()`（默认全 `false`，新后端不实现即被拒绝）；
- 路由层用 `Router::checkEncodeParams()` 在**真正编码之前**判定，缺项时返回
  `UnsupportedFormat` 且 `message` 点名具体项（`encode params not supported: progressive`）；
- `Router::encodeCapabilities()` 取**并集**：自动模式下会跳过不支持的后端，只要顺序里有一个
  能做，请求就成立。取交集会让「CPU 支持、GPU 不支持」的常用组合在 UI 上被误判成不支持；
- `image_codec_offload_encode_capabilities()` 是同一张表的 C API 出口：自动句柄给并集，
  `create_forced` 句柄给该后端自己的表。

**GPU 后端目前不声明任何进阶参数**（能力表默认全 0，只有 CPU 实现期间才置 1）。
基础参数（JPEG 的 quality/subsampling、TIFF 的 none/LZW/strip-tiled、PNG 的
compression/filter）不在能力表里——它们是三个后端一直就有的共同底线。进阶参数一律由
CPU 表达，GPU lane 会被能力表挡掉而不是「照收但忽略」——这是刻意的：静默丢参数会产出
「文件能打开、看着也正常，只有该有的 SOF2/Predictor/DPI 没了」的产物。

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
| `IMAGECODEC_OFFLOAD_WITH_PLATFORM` | `OFF` | 构建 `ImageCodecPlatform`（平台抽象 + Intel oneVPL 硬件 JPEG / TIFF JPEG strips）。**只加库，不动路由后端表与 `ico`** |
| `IMAGECODEC_OFFLOAD_WITH_VULKAN` | `OFF` | 构建 Vulkan 后端（JPEG/TIFF/PNG + 指标）。需要 GLSL 编译器（`glslc` 或 `glslangValidator`） |
| `IMAGECODEC_OFFLOAD_BUILD_CLI` | `ON` | 构建 `ico` |
| `IMAGECODEC_OFFLOAD_BUILD_TESTS` | `OFF` | 构建并注册 ctest（`ico_test` + 各子项目测试） |

### `ImageCodecPlatform` 的依赖（`_WITH_PLATFORM=ON` 时才相关）

构建期只需要 **oneVPL 头**（submodule，不链接、不需要 SDK 安装包）：

```bash
git submodule update --init --recursive   # 3rdparty/libvpl（头）、3rdparty/vpl-gpu-rt（参考，不编译）
```

运行期才需要硬件栈（编译机可以完全不装）：

| 平台 | 需要 | 说明 |
| --- | --- | --- |
| Linux | `libvpl2` + `libmfx-gen1.2`（vpl-gpu-rt，提供 `libmfx-gen.so.1.2`）+ `intel-media-va-driver` | `sudo apt install libvpl2 libmfx-gen1.2 intel-media-va-driver`；`libva2`/`libva-drm2` 由 apt 带上；权限只要 `/dev/dri/renderD*` 可读写（logind 自动 ACL），不需要 root、不需要 `render` 组 |
| Windows | Intel 显卡驱动（自带 `libvpl.dll` + `libmfx-gen.dll`） | 不需要 oneVPL SDK；驱动装好即可 |

子项目自测 CLI（`OIC_BUILD_CLI`，父项目内默认关闭）另需**可选**的 `libjpeg` / `libtiff`：
`libjpeg` 提供软件参考编码器（没有 Intel 硬件也能验证 TIFF 容器），`libtiff` **只用于解码校验**。
两者缺失时对应自测项 SKIP，不影响库构建。

明确不需要：D3D11/DXGI、OpenCL/CUDA 运行库、X server（纯计算，可无显示器）、root 权限。
依赖细节与实测环境见 `libs/ImageCodecPlatform/README.md` 的「依赖清单」。

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
  它目前**没有**接进路由后端表：`ico backends` 里看不到它，打开
  `IMAGECODEC_OFFLOAD_WITH_PLATFORM` 只多编译出库（`OpenCLImageCodec::Jpeg::oneVPL`、
  `OpenCLImageCodec::Tiff`）与自测 CLI（子项目的 `OIC_BUILD_CLI`，父项目内默认关闭）。
- Intel 硬件 JPEG **不是**「TIFF 也有硬件单元」：TIFF 侧只有容器，压缩走
  「硬件 JPEG strips（Compression=7）」。TIFF 容器由本仓库自写（IFD / JPEGTables），
  libtiff 只用于测试端解码校验——它写 JPEG 会重新编码 strip，没有入口接收已编好的码流。
- `ImageCodecVulkan` 可通过 `add_subdirectory` 被上层复用；编解码走
  `VulkanImageCodec::CodecApi`（平台无关），GL 互操作走 `VulkanImageCodec::Api`
  （仅 Windows），不要复制其 Win32 handle/GL 导入实现。
- Vulkan 着色器在 configure 阶段编译成 SPIR-V 再嵌入头文件（`cmake/spv_to_header.cmake`），
  `.spv` 是构建产物、不提交进仓库；**新增着色器必须重新 configure**。
- CUDA / OpenCL / Vulkan 与 CPU 参考的画质指标必须逐项给出**同一个数字**（golden 常量锁在
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
