<!--
 * File: README.md
 * Brief: Image.Codec.Vulkan.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
-->

# Image.Codec.Vulkan

Vulkan compute 库：**编解码与画质指标是平台无关的**；只有「Vulkan→OpenGL
零拷贝互操作」那一段是 Windows 专用（WGL + `VK_KHR_external_memory_win32`）。

## 特性
- `Image.Codec.Vulkan::Core`：Vulkan 设备/上下文、可导出 Buffer/Image、compute pipeline（平台无关）。
- `Image.Codec.Vulkan::Common`：编解码共用层——1..4 通道 `ImageBuffer`、host-visible
  缓冲、显存预算记账（`VK_EXT_memory_budget` 查空闲显存）、storage-buffer compute pipeline（平台无关）。
- `Image.Codec.Vulkan::Jpeg`：baseline JPEG。容器（marker / 表 / MCU 布局）与 Huffman 位流在 CPU，
  DCT/IDCT、RGB↔YCbCr、色度上下采样在 GPU；支持 4:4:4 / 4:2:2 / 4:2:0 与灰度。
- `Image.Codec.Vulkan::Tiff`：TIFF 容器（LE/BE 读取、strip/tiled）+ LZW 编解码（CPU）+
  `Predictor=2` 水平差分（GPU）。
- `Image.Codec.Vulkan::Png`：行滤波（GPU，自适应或固定 0..4）+ DEFLATE/inflate（CPU，zlib）。
- `Image.Codec.Vulkan::CodecApi`：平台无关的编解码 C API（`image_codec_vulkan_*`），
  供路由层/调用方选用，三种格式 + 指标都从这里进。
- `Image.Codec.Vulkan::Metrics`：MSE / PSNR / SSIM / MS-SSIM，GPU 累加用 double，
  与 CUDA / OpenCL 后端**逐位一致**（golden 常量锁在父项目 `tests/ico_test.cpp`）。
- `Image.Codec.Vulkan::GlInterop`（Windows）：`wglGetProcAddress` 手写加载 `GL_EXT_memory_object(_win32)`，将 win32 handle 导入 GL buffer/texture。
- `Image.Codec.Vulkan::Api`（Windows）：`GlImagePipeline` 一步完成 compute→GL。
- Windows 上全程 CPU 零搬运：Vulkan `VK_KHR_external_memory_win32` 导出 → GL `glImportMemoryWin32HandleEXT` 导入同一块显存。

## 平台划分（为什么 Linux 也能用）
| 模块 | Windows | Linux / 其它 |
| --- | --- | --- |
| Core / Common / Metrics / Jpeg / Tiff / Png / CodecApi | ✅ | ✅ |
| GlInterop / Api（GL 互操作） | ✅ | 不构建、不导出 |

`VK_USE_PLATFORM_WIN32_KHR` 只在 `_WIN32` 下定义；非 Windows 上没有
`vulkan_win32.h`，任何无条件引用都会让整个仓库连 Vulkan 头都包含不进来。

## 依赖
- vcpkg：`vulkan`（headers + loader）、`zlib`（PNG 的 deflate/inflate）
- GLSL 编译器：构建期需要 `glslc`（Vulkan SDK）或 `glslangValidator`
  （Linux 发行版通常只提供后者，如 `apt install glslang-tools`）。
  两者都只用来生成 SPIR-V，**不提交 `.spv`**，运行期也不需要它们。
- 指标需要设备支持 `shaderFloat64`：double 累加是对拍的前提。**只影响指标**——
  设备没有该特性时编解码（JPEG/PNG/TIFF 全是 fp32）照常可用，指标调用返回
  `BackendUnavailable` 并附原因，由上层回退 CPU 指标，绝不悄悄退化成 float。
  （只有核显的机器不会因为一个可选算子被整块判死。）
- Windows GL 互操作另需：`opengl32.lib` / `user32.lib` / `gdi32.lib`，驱动支持
  `GL_EXT_memory_object` / `GL_EXT_memory_object_win32`（NVIDIA/AMD 现代桌面驱动均支持）。

## 构建
```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVIC_BUILD_TESTS=ON
cmake --build build --config Debug
```

## 测试
```bash
ctest --test-dir build -C Debug --output-on-failure   # 或
python tests/run_test.py
```
退出码约定：0=PASS、2=SKIP（环境不支持扩展）、1=FAIL。

## 结构
```
include/Image/Codec/Vulkan/vic.hpp            # 公共头（Vulkan 侧类，含 GL 互操作）
include/Image/Codec/Vulkan/image_codec_vulkan.h  # 平台无关编解码 C API
src/libs/Image.Codec.Vulkan.Core/           # 设备/可导出资源/compute（平台无关）
src/libs/Image.Codec.Vulkan.Common/         # ImageBuffer / 缓冲 / 显存预算 / pipeline（平台无关）
src/libs/Image.Codec.Vulkan.Metrics/        # MSE / PSNR / SSIM / MS-SSIM（GPU + CPU 参考）
src/libs/Image.Codec.Vulkan.Jpeg/           # baseline JPEG：容器/Huffman(CPU) + DCT/颜色(GPU)
src/libs/Image.Codec.Vulkan.Tiff/           # TIFF 容器 + LZW(CPU) + 预测器(GPU)
src/libs/Image.Codec.Vulkan.Png/            # PNG 行滤波(GPU) + DEFLATE(CPU)
src/libs/Image.Codec.Vulkan.CodecApi/       # 平台无关 C API（三种格式 + 指标）
src/libs/Image.Codec.Vulkan.GlInterop/      # GL 扩展加载 + 导入（仅 Windows）
src/libs/Image.Codec.Vulkan.Api/            # GlImagePipeline 高层封装（仅 Windows）
cmake/spv_to_header.cmake                   # .spv -> uint32_t 数组头（各模块共用）
tests/                                      # C++ 测试（codec_api / 指标 / PNG / TIFF；GL 互操作仅 Windows）
```

**GPU 与后端自带的 CPU 参考逐位一致**（`tests/codec_api_test.cpp` 做字节级比对），
三条约束缺一不可：颜色变换用 libjpeg 的 16 位定点系数（浮点 BT.601 会被驱动 FMA 合并，
1 ulp 经 `(a+b+c+d+2)>>2` 放大成 ±1 色度）；DCT 累加用 GLSL `precise` + CPU 侧
`-ffp-contract=off`；量化舍入带 1e-3 平局偏置（DC 经常精确落在 `n+0.5`）。

色度下采样的抽取比例由采样因子推出（`h_ratio = max_h/h_samp`、`v_ratio = max_v/v_samp`），
4:2:2 是 **2x1** 而不是 2x2——写死 2x2 会让 4:2:2 的下半幅颜色被摊平（文件能开、亮度正常）。
