<!--
 * File: README.md
 * Brief: Image.Codec.Offload.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
-->

# Image.Codec.Offload

大图（GB 级 BMP/TIFF/PNG）图像编解码卸载仓库：四个后端（CUDA / OpenCL / Vulkan /
Intel oneVPL）加上一个**路由层**，调用方只需说「压这张图」，由路由层选后端、失败时回退，
并给出每一次回退的原因。CPU 基线（libtiff / libpng / libjpeg-turbo）永远可用，同时充当
GPU 后端的像素级参照。

后端不再是嵌套子模块：它们与路由层一起放在 `src/libs/` 下，同一次提交、同一份版本。

## 快速开始

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DIMAGECODEC_OFFLOAD_BUILD_TESTS=ON
cmake --build build -j

./build/src/cli/ico backends                       # 各后端是否 ready
./build/src/cli/ico encode in.bmp out.jpg --quality 92
./build/src/cli/ico decode out.jpg back.bmp
./build/src/cli/ico compare in.bmp back.jpg        # 三后端指标并排
```

CUDA 后端加 `-DIMAGECODEC_OFFLOAD_WITH_CUDA=ON`（需要 CUDA Toolkit / nvJPEG）。
NVIDIA 厂商层（nvJPEG / nvJPEG2000 / nvTIFF，只加库、不进路由表）加
`-DIMAGECODEC_OFFLOAD_WITH_NVIDIA_PLATFORM=ON`；它不需要 nvcc，选装自测 CLI 见
`docs/README.md` 的「`Image.Codec.Platform.NVIDIA` 的依赖」。

## 内容

| 部分 | 说明 |
| --- | --- |
| `include/Image/Codec/Offload/` | 对外 C API `image_codec_offload_*` |
| `src/libs/Image.Codec.Offload.*` | 路由层：Core（类型/指标 CPU 参考）、Common（BMP）、Cpu（基线）、Router（选择与回退）、Api |
| `src/cli/` | `ico`：backends / info / encode / decode / compare |
| `src/libs/Image.Codec.CUDA` | NVIDIA 后端：nvJPEG、GPU LZW、GPU 行滤波、GPU 指标、调参库 |
| `src/libs/Image.Codec.OpenCL` | OpenCL 后端：混合 JPEG、GPU LZW |
| `src/libs/Image.Codec.Platform.Intel` | 平台抽象与 Intel oneVPL 硬件后端 |
| `src/libs/Image.Codec.Platform.NVIDIA` | NVIDIA 厂商后端：nvJPEG、nvJPEG2000（JP2）、nvTIFF（容器 + LZW/Deflate/JPEG/PackBits） |
| `src/libs/Image.Codec.Vulkan` | Vulkan compute 编解码：TIFF（Predictor=2）、PNG 行滤波、baseline JPEG、画质指标 |
| `tests/` | 端到端测试：往返、指标 golden、三后端对拍、灰度保真、回退原因 |

## 设计要点

- **回退边界**：后端不可用 / 不支持该格式 / 资源不足 → 换下一个；编解码失败 / IO /
  内部错误 → 不拦路但记账，全失败时作为最终报错；参数非法 → 立刻中止。
- **通道保真**：1 通道进 1 通道出。做不到的后端明确返回 `unsupported-format`，
  不允许「升维后假装成功」。
- **三个后端同一组指标数字**：MSE / PSNR / SSIM / MS-SSIM 的定义与常数统一，
  golden 常量锁在测试里。
- **显存可控**：可查询、可设上限；资源不足与真失败用不同状态码。

详细说明见 [`docs/README.md`](docs/README.md)。
