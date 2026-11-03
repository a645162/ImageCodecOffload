<!--
 * File: README.md
 * Brief: OpenCL-Image-Codec.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
-->

# OpenCL-Image-Codec

通过 vcpkg 获取 OpenCL，CMake 子项目组织，独立构建；对外提供稳定 C API（另附 C++ 包装）与测试 CLI。

## 特性

三个格式都是「**只把最贵的一步放 GPU**」，容器与熵编码留在 CPU：

- **JPEG**（baseline，SOF0，8-bit）
  - GPU（OpenCL）：**混合策略**——Huffman 熵编解码在 CPU，DCT/IDCT、量化/反量化、YCbCr↔RGB、上下采样在 GPU。支持 4:4:4 / 4:2:0 / 4:2:2 / 灰度（**编解码双向**）。
- **TIFF**（6.0，Compression=1 none / 5 LZW，strip/tile）
  - 编码：LZW 在 GPU（strip/tile 并行 kernel，block 自适应）。
  - 解码：LZW 在 CPU（对齐 libtiff 语义）。GPU 并行 LZW 解码已探索并评估为不优于 CPU（见 `src/tiff/README.md`）。
- **PNG**（8-bit，非隔行，color type 0/2/4/6）
  - GPU（OpenCL）：行滤波与自适应滤波器选择（两个 kernel：逐行滤波 + 每行 5 种 filter 的残差绝对值和）。
  - CPU：chunk/CRC 与 zlib deflate/inflate；GPU 不可用时回退同一套 CPU 参考实现，并记录原因。
- **通道保真**：1/2/3/4 通道（灰度 / 灰度+A / RGB / RGBA）在 TIFF、PNG 与 C API
  全链路原样往返，不做隐式升维、不丢 Alpha。JPEG 没有 Alpha 样本，2/4 通道返回
  `UNSUPPORTED_FORMAT`（不降维冒充成功）。

## 构建

依赖：vcpkg（`opencl` 端口）、CMake ≥3.16、MSVC（Ninja 生成器）。

```bash
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

- OpenCL 由自带 `vcpkg.json`（manifest 模式）从 vcpkg 获取。
- 选项：`OIC_BUILD_CLI`（默认 ON）、`OIC_BUILD_EXAMPLES`（默认 ON）、`OIC_BUILD_TESTS`（默认 OFF）。

## CLI 用法（`build/cli/oic.exe`）

```
image_codec list-devices
image_codec info <in>
image_codec decode <in> <out> [--backend opencl|cpu]
image_codec encode <in> <out> [--backend ...] [--jpeg-quality N]
                       [--tiff-compression 1|5] [--rows-per-strip N]
                       [--png-level 0..9] [--png-filter -1|0..4]
image_codec bench <in> [--iterations N] [--backend ...]
```

示例：
```bash
image_codec encode in.bmp out.tiff --backend opencl --tiff-compression 5  # TIFF LZW GPU 编码
image_codec decode out.tiff back.bmp --backend cpu                        # TIFF 解码
image_codec encode in.bmp out.jpg --backend opencl --jpeg-quality 90      # JPEG OpenCL 编码
image_codec decode out.jpg back.bmp --backend opencl                      # JPEG 解码
image_codec encode in.bmp out.png --backend opencl --png-filter -1        # PNG 自适应行滤波
image_codec decode out.png back.bmp --backend opencl                      # PNG 解码
```

## C API（`include/Image/Codec/OpenCL/oic.h`）

```c
oic_codec *c; oic_codec_create(&c, OIC_BACKEND_OPENCL);
oic_image img; oic_codec_decode(c, OIC_FORMAT_JPEG, buf, len, &img);  // img.data RGB
oic_codec_encode(c, OIC_FORMAT_TIFF, &img, &params, out, &out_len, cap);
oic_image_free(&img); oic_codec_destroy(c);
```

C++ 包装（`oic.hpp`）：`oic::Codec`（RAII，抛 `oic::Error`）、`oic::Image`、`oic::DeviceList`。
示例：`examples/c_example.c`（纯 C）、`examples/cpp_example.cpp`。

## 目录结构

```
include/Image/Codec/OpenCL/  对外 C/C++ API 头（image_codec.h / image_codec.hpp）
src/libs/Image.Codec.OpenCL.Core    平台抽象 + OpenCL 设备/上下文/kernel 构建
src/libs/Image.Codec.OpenCL.Common  ImageBuffer + BMP 中转（8-bit 灰度 / 24-bit RGB）
src/libs/Image.Codec.OpenCL.Tiff    TIFF 容器 + LZW 编码(GPU)/解码(CPU)
src/libs/Image.Codec.OpenCL.Jpeg    baseline JPEG 混合编解码（OpenCL）
src/libs/Image.Codec.OpenCL.Png     PNG 容器(zlib) + GPU 行滤波
src/libs/Image.Codec.OpenCL.Metrics MSE / PSNR / SSIM / MS-SSIM
src/libs/Image.Codec.OpenCL.Api     C API 实现（buffer ↔ 临时文件桥接）
src/cli/                   image_codec 可执行（子命令 info/decode/encode/bench）
examples/                  纯 C / C++ 示例
```

## 已知限制

- **JPEG 彩色解码**：与 libjpeg 最大差 ≤3 灰阶（0.5% 像素，浮点 IDCT vs libjpeg 整数 IDCT）；灰度/8×8 像素级一致。
- **JPEG**：编码/解码支持 1 分量（灰度）与 3 分量；渐进式/非 8-bit/16-bit 量化表拒绝。
- **TIFF 解码**：8-bit chunky（LZW/none），spp=1（BlackIsZero）与 spp=3（RGB）。
- **PNG**：仅 8-bit 非隔行；16-bit / 隔行 / 调色板（color type 3）拒绝。
- **Intel NEO**：单 work-item 输入 >~200KB 可能 `CL_OUT_OF_RESOURCES`（编码已按 `block_cap` 自适应）。

## 许可证

