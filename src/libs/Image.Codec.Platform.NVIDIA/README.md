<!--
 * File: README.md
 * Brief: Image.Codec.Platform.NVIDIA.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-11-03
-->

# Image.Codec.Platform.NVIDIA

NVIDIA **厂商层**编解码（nvJPEG / nvJPEG2000 / nvTIFF）。与 `Image.Codec.Platform.Intel`
完全对称：它回答的是「这台机器的 NVIDIA 栈能做什么」，并把厂商库包成本仓库统一的语义
（宿主侧 `ImageBuffer` 进出、显式设备与 stream、显存记账、带上下文的错误文本）。

## 这是什么

```
Image.Codec.Platform.NVIDIA::Core    设备枚举 / 显式 stream / 显存记账 / 设备缓冲 / 错误文本
        ▲
Image.Codec.Platform.NVIDIA::Jpeg    nvJPEG：baseline JPEG（1 / 3 通道）
Image.Codec.Platform.NVIDIA::Jpeg2K  nvJPEG2000：JPEG 2000，默认写 JP2 容器（1 / 3 通道）
Image.Codec.Platform.NVIDIA::Tiff    nvTIFF：TIFF 容器 + NONE / LZW / Deflate / JPEG / PackBits
        ▲
Image.Codec.Platform.NVIDIA::All     以上三者的聚合入口（只聚合本次构建真实存在的组件）
inp_selftest                         自测 CLI（OIC_BUILD_CLI，默认 OFF）
```

三条硬性前提：

- **不需要 nvcc**：三个厂商库都是普通 C 库 + `cuda_runtime_api.h`，本工程不
  `enable_language(CUDA)`、不编 `.cu`、不涉及 sm 架构。没有 GPU 的机器也能 configure
  与编译；运行期由 `available()` / 设备枚举如实报告。
- **组件可缺**：nvJPEG 随 CUDA Toolkit 提供，nvJPEG2000 与 nvTIFF 是**独立安装的包**。
  缺哪个只打印一行 `STATUS` 并把该 codec 标为不可用，绝不 FATAL、绝不用别的实现冒充。
- **通道保真**：1 通道进 1 通道出（JPEG 灰度为 `CSS_GRAY`，nvJPEG 没有单通道编码输入，
  所以宿主侧复制成 R=G=B）；JPEG / TIFF 编码只接受 1 与 3 通道，2/4 明确拒绝；
  TIFF 解码保留文件里的 1..4 通道。

## 依赖清单

### 1. 构建期（必需）

| 依赖 | 形式 | 用途 | 怎么来 |
| --- | --- | --- | --- |
| CMake | ≥ 3.16 | 构建 | 系统包 |
| C++17 编译器 | GCC 9+ / Clang 10+ / MSVC 2019+ | 源码只用 C++17 | 系统包 |
| CUDA Runtime | `CUDA::cudart`（Toolkit 头 + 库） | `cuda_runtime_api.h`、显式 stream、设备内存 | CUDA Toolkit |
| `nvjpeg` | `CUDA::nvjpeg`（Toolkit 内） | baseline JPEG 编解码 | CUDA Toolkit |
| `nvjpeg2k` | `nvjpeg2k.h` + `libnvjpeg2k` | JPEG 2000 / JP2 编解码 | 发行版的 `libnvjpeg2k*` 包或 NVIDIA 官方包 |
| `nvtiff` | `nvtiff.h` + `libnvtiff` | TIFF 容器与压缩 | 发行版的 `libnvtiff*` 包或 NVIDIA 官方包 |

后两者用 `find_path` + `find_library` 成对探测，**不写死版本目录**（`/usr/include` 上
是 alternatives 符号链接，版本目录随包变化）。缺失时：

```text
Image.Codec.Platform.NVIDIA: 未找到 nvTIFF（头 nvtiff.h / 库 nvtiff），TIFF 将报告 UNSUPPORTED
```

### 2. 运行期

NVIDIA 驱动 + 对应运行库（`libnvjpeg2k.so` / `libnvtiff.so`）。自测机器实测：
RTX 4070（`sm_89`）、CUDA 13.3、nvJPEG 13.2、nvJPEG2000 0.12、nvTIFF 0.9。

## 构建与自测

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DOIC_BUILD_CLI=ON
cmake --build build -j
./build/src/cli/inp_selftest
```

组件开关（默认全 ON，缺失时自动关）：

| 选项 | 对应目标 |
| --- | --- |
| `OIC_NVIDIA_WITH_JPEG` | `Image.Codec.Platform.NVIDIA::Jpeg` |
| `OIC_NVIDIA_WITH_JPEG2K` | `Image.Codec.Platform.NVIDIA::Jpeg2K` |
| `OIC_NVIDIA_WITH_TIFF` | `Image.Codec.Platform.NVIDIA::Tiff` |

作为主项目的子目录复用时由 `IMAGECODEC_OFFLOAD_WITH_NVIDIA_PLATFORM`（默认 OFF）
挂载，此时不带 CLI。

自测判据（**无损项必须逐字节相等**，有损项才用 PSNR 阈值）：

- nvJPEG：RGB ≥ 32 dB（实测 51.4）、灰度 ≥ 32 dB（实测 71.4）；
- nvJPEG2000：`quality = 0` 无损逐字节相等；有损 ≥ 30 dB（实测 59~62）；
- nvTIFF：NONE / LZW / Deflate × RGB / 灰度 逐字节相等。

## 两个必须知道的坑

1. **nvTIFF 的编码输入没有 pitch**。`nvtiffEncodeParamsSetInputs` 只收裸指针，库按
   `width × bits_per_pixel` 紧密连续读行。用 `cudaMallocPitch` 分配并按 pitch 上传，
   会得到「几何正确、通道正确、像素全错」的产物（连 Compression=NONE 都不是无损）。
   编码侧必须紧密打包；解码侧 `nvtiffImage_t` 自带 `plane_pitch_bytes`，可以用 pitch。
2. **`nvtiffDecode` 之后必须同步 stream 再销毁 decoder**：decoder 复用内部 scratch，
   GPU 还在跑的时候销毁会在驱动里炸，且现场看不出与本调用有关。

## 与 `Image.Codec.CUDA` 的分工

`Image.Codec.CUDA` 是本仓库的**计算后端**（自己写 kernel，走路由表）；本工程是**厂商层**
（直接调 NVIDIA 的库，不进路由表）。两者都可以单独构建，开关互不依赖。
`Image.Codec.CUDA.NvJpeg` 是 CUDA 后端内部为它的 C API 服务的另一份 nvJPEG 包装，
暂未合并——合并要求 CUDA 工程依赖本工程，会破坏「两个工程可分别配置」这一点。
