<!--
 * File: README.md
 * Brief: Image.Codec.Platform.Intel.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
-->

# Image.Codec.Platform.Intel

平台抽象与**厂商专用**（Intel 媒体引擎）编解码后端。它不是「又一个通用后端」，
而是把「机器上到底有没有硬件编解码单元」这件事问清楚，并把硬件能做的事接进本仓库。

## 这是什么

```
Image.Codec.Platform.Intel::Core      exe_dir / 单调时钟 / 休眠（Windows + POSIX 两套实现）
        ▲
Image.Codec.Platform.Intel::Jpeg::oneVPL  硬件单元探测 + baseline JPEG 编解码（oneVPL）
        ▲
Image.Codec.Platform.Intel::Tiff          TIFF 6.0 容器：预编码 JPEG strips（Compression=7）/ 未压缩
        ▲
icp_selftest                    自测 CLI（OIC_BUILD_CLI，默认 OFF）
```

三条硬性前提：

- **Linux + Windows 同一个代码路径**：运行库在运行期动态加载（Windows
  `LoadLibraryA("libvpl.dll")`，Linux `dlopen("libvpl.so.2")`），不需要导入库；
  帧只走**系统内存**（`MFX_IOPATTERN_*_SYSTEM_MEMORY`），不建 D3D11 设备、不碰 VA-API 句柄。
- **TIFF 没有硬件单元**：硬件只出 JPEG 码流，TIFF 是容器。所以 TIFF 路线是
  「硬件 JPEG → TIFF JPEG strips（Compression=7）」，容器自己写（libtiff 的 JPEG codec
  只会拿像素重新编码，没有入口接收已经编好的条带）。
- **探测要能解释失败**：把「运行库没装 / 装了但没有实现 / 有实现但不支持 JPEG」
  三种情况分开报告，上层才能给出可操作的提示。

## 依赖清单

分三层：**构建期**（编译这个仓库要什么）、**运行期**（跑硬件路径要什么）、
**可选**（只有自测 CLI 才需要）。每一层都给出「装什么 / 怎么装 / 怎么确认」。

### 1. 构建期（必需）

| 依赖 | 版本 / 形式 | 用途 | 怎么来 |
| --- | --- | --- | --- |
| CMake | ≥ 3.16 | 构建 | 系统包 |
| C++17 编译器 | GCC 9+ / Clang 10+ / MSVC 2019+ | 全部源码只用 C++17，**不需要 C++20/23** | 系统包 |
| `3rdparty/libvpl` | git submodule（<https://github.com/intel/libvpl>） | **唯一必需的头文件来源**：`mfxvideo.h` / `mfxdefs.h` / `mfxstructures.h` / `mfxdispatcherprefixedfunctions.h` 等，只用头，不链接 | `git submodule update --init --recursive` |
| `3rdparty/vpl-gpu-rt` | git submodule（<https://github.com/intel/vpl-gpu-rt>） | **不参与构建**，只是运行时实现的参考与版本对照 | 同上（一次性递归即可） |

Linux 额外：`${CMAKE_DL_LIBS}`（glibc < 2.34 上是 `-ldl`，之后已并入 libc）。
CMake 会自动处理，**不需要**手动加 `-ldl`。

明确**不需要**：`libvpl-dev` / `libmfx-dev` 的 `.so` 导入库、pkg-config 文件。
oneVPL 在运行期用 `dlopen("libvpl.so.2")` / `LoadLibraryA("libvpl.dll")` 动态加载，
所有函数指针都从 dispatcher 取（见 `vpl_dyn.h`）。所以**编译机可以完全不装 oneVPL**，
只要 submodule 头在就能编。

`CMakeLists.txt` 里有一道显式检查：`3rdparty/libvpl/api/vpl/mfxvideo.h` 不存在时直接
`FATAL_ERROR` 提示「请先执行 git submodule update --init --recursive」，避免只看到
一堆 `mfxU16 未定义`。

### 2. 运行期（跑硬件路径必需）

#### Linux

需要**三件套**，缺一不可：

| 包 | 提供 | 缺了会怎样 |
| --- | --- | --- |
| `libvpl2` | `libvpl.so.2`（**dispatcher**，只负责找实现、转发调用） | `vpl_probe()` 报「未找到 oneVPL 运行库」，硬件项 SKIP |
| `libmfx-gen1.2` | `libmfx-gen.so.1.2`（**vpl-gpu-rt**，真正驱动 GPU 媒体引擎的实现） | 实现列表为空，报「运行库已加载但没有任何实现」 |
| `intel-media-va-driver` | `iHD_drv_video.so`（VA-API 驱动，`libmfx-gen` 通过它说话） | VA-API 初始化失败，实现枚举不出来 |

```bash
sudo apt install libvpl2 libmfx-gen1.2 intel-media-va-driver
```

`libva2` / `libva-drm2` 是 `libmfx-gen1.2` 的依赖，apt 会自动带上；
`intel-media-va-driver` 在 Ubuntu 22.04+ 已经是默认 iHD 驱动。

**包名对照**（不同发行版叫法不同，认「提供哪个 .so」而不是认包名）：

| 发行版 | dispatcher | vpl-gpu-rt（Intel 实现） | VA-API 驱动 |
| --- | --- | --- | --- |
| Ubuntu / Debian | `libvpl2` | `libmfx-gen1.2`（universe） | `intel-media-va-driver` |
| Fedora / RHEL | `oneVPL` | `oneVPL-intel-gpu` | `intel-media-driver` |
| Arch | `onevpl` | `onevpl-intel-gpu` | `intel-media-driver` |
| openSUSE | `libvpl2` | `onevpl-intel-gpu` | `intel-media-driver` |
| Windows | 随 Intel 显卡驱动安装 `libvpl.dll` | 随驱动安装 `libmfx-gen.dll` | 驱动内部（D3D11/Media SDK） |

装完的确认命令（三条都要有输出）：

```bash
ldconfig -p | grep -E 'libvpl\.so\.2|libmfx-gen\.so\.1\.2'
ls /usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so
ls -l /dev/dri/renderD*
```

**注意 `libmfx-gen.so.1.2` 没有 `libmfx-gen.so.1` 这个软链**：Ubuntu 26.10 的
`libmfx-gen1.2` 只装 `libmfx-gen.so.1.2 -> libmfx-gen.so.1.2.16`。dispatcher
`libvpl.so.2` 内部硬编码的就是 `libmfx-gen.so.1.2`，所以不要自己改成 `.so.1`。

**设备权限**：需要能打开 Intel 的 render node（`/dev/dri/renderD128` 或 `renderD129`）。
现代 systemd-logind 会给当前登录用户自动加 ACL，`getfacl /dev/dri/renderD129`
能看到 `user:<你>:rw-` 即可，**不需要**把用户加进 `render` 组、也不需要 root。
只有 SSH 无 logind 会话的机器才需要 `sudo usermod -aG render $USER` 并重新登录。

#### Windows

| 依赖 | 提供 | 备注 |
| --- | --- | --- |
| Intel 显卡驱动（Intel Graphics Driver，含 Media 组件） | `libvpl.dll` + `libmfx-gen.dll` | 驱动安装时装进 `System32`，**不要**手工拷 DLL |
| VC++ 运行库 | `vcruntime140.dll` 等 | 与仓库其它部分一致，由安装器/发布包负责 |

Windows 侧**不编译、不链接** oneVPL SDK：只用 submodule 里的头，运行期
`LoadLibraryA("libvpl.dll")`。所以不需要安装 Intel oneVPL SDK，也不需要配 `INCLUDE`/`LIB`。
本子项目在 Windows 上**只做编译验证，未实机验证硬件路径**（见下方实测环境）。

### 3. 可选（只有自测 CLI 需要）

`OIC_BUILD_CLI=ON` 时 `src/cli/CMakeLists.txt` 会 `find_package(JPEG QUIET)` /
`find_package(TIFF QUIET)`，都是**可选**：找不到时对应自测项 SKIP，整个程序仍可构建。

| 包 | CMake 目标 | 用途 | 缺了会怎样 |
| --- | --- | --- | --- |
| `libjpeg`（Debian/Ubuntu 包名 `libjpeg-dev`，底层实现是 libjpeg-turbo） | `JPEG::JPEG` | `--software` 的参考编码器，让**没有 Intel 硬件的机器也能验证 TIFF 容器** | `tiff --software` 与容器自测 SKIP |
| `libtiff`（`libtiff-dev`） | `TIFF::TIFF` | **只用于解码校验**（写文件自己实现，libtiff 的 JPEG codec 没有「接收已编码条带」的入口） | TIFF 写出后的 PSNR 校验 SKIP |

```bash
sudo apt install libjpeg-dev libtiff-dev    # 仅自测 CLI 需要
```

库本身的三个 target（`Platform` / `Jpeg.oneVPL` / `Tiff`）**不依赖 libjpeg、libtiff**：
父仓库 `IMAGECODEC_OFFLOAD_WITH_PLATFORM=ON` 时不会因为它们缺失而构建失败。

### 4. 明确不需要的东西

| 常被误以为需要 | 实际情况 |
| --- | --- |
| D3D11 / DXGI 设备 | 已彻底移除。帧只走系统内存，Windows 与 Linux 同一份代码 |
| OpenCL / CUDA / Vulkan 运行时 | 与这些后端无关，本子项目不链接任何 GPU 计算栈 |
| X server / Wayland / 显示输出 | 纯计算路径，无窗口、无 GL。服务器上只挂核显（无显示器）也能跑 |
| root 权限 | 只要 render node 可读写即可 |
| Intel oneVPL SDK 安装包 | 头文件来自 submodule；运行库是系统包/驱动 |
| `vpl-gpu-rt` 源码编译 | Ubuntu/Debian 直接用 `libmfx-gen1.2` 二进制包即可 |

### 5. 实测环境

| 项 | 值 |
| --- | --- |
| CPU / iGPU | Intel Core i9-12900K / **Intel UHD Graphics 770**（Alder Lake-S GT1，`8086:4680`） |
| 系统 | Ubuntu 26.10，kernel 7.3.0-10-generic |
| 驱动栈 | i915（渲染节点 `/dev/dri/renderD129`），显示输出在另一块 NVIDIA RTX 4070 上 |
| dispatcher | `libvpl2` 1:2.16.0-1 → `libvpl.so.2.16` |
| Intel 实现 | `libmfx-gen1.2` 26.1.2-1.1 → `libmfx-gen.so.1.2.16` |
| VA-API 驱动 | `intel-media-va-driver` 26.2.4 → `iHD_drv_video.so` |
| 结果 | `units` 报 `jpeg_encode=yes jpeg_decode=yes`；`selftest --require-hardware` 全绿（JPEG 往返 **43.83 dB**，硬件 JPEG strips 的 TIFF **27.01 dB**） |

## 硬件单元（oneVPL 实现）探测

```bash
./icp_selftest units
```

```
oneVPL 运行库：libvpl.so.2
实现数量：1
  #0 mfx-gen [hardware] vendor=Intel device=4680/1 accel=VA-API api=2.16 jpeg_encode=yes jpeg_decode=yes enc_in=[NV12,YV12,YUY2,RGB4,BGR4,400P] dec_out=[NV12,RGB4,YUY2]
  Intel 硬件单元：有（JPEG 编码=yes，解码=yes）
```

没有 `vpl-gpu-rt` 时实现列表为空，输出会明确指出这一点，而不是在 `Init` 处报一个看不懂的
`MFX_ERR_NOT_FOUND`。注意 Linux 上 `libvpl` 只是 **dispatcher**，真正干活的是
`vpl-gpu-rt` 提供的 `libmfx-gen.so.1.2`（Ubuntu/Debian 包名 `libmfx-gen1.2`），
另外还需要 `intel-media-va-driver` 提供 `iHD_drv_video.so`。

`vpl_probe()` 的实现见 `src/libs/Image.Codec.Platform.Intel.Jpeg.oneVPL/src/vpl_device.cpp`：
枚举 `mfxImplDescription`（厂商 / ImplName / API 版本 / 加速栈 / 编解码能力），
优先选 Intel(0x8086) + hardware，并且**绝不**把软件实现当成硬件来跑。

## 硬件 JPEG 编解码

- 编码：`MFXMemory_GetSurfaceForEncode` 取运行库内部系统内存 surface，
  `FrameInterface->Map(MFX_MAP_WRITE)` 之后才有 `Data.Y/UV`（dispatcher 不提供
  `MFXMemory_MapFrame`），按 pitch 拷入 NV12、`Unmap` 再送 `EncodeFrameAsync`。
  漏掉 `Unmap` 会得到 `MFX_ERR_LOCK_MEMORY`，看起来却像「surface 尺寸不足」。
- 解码：`DecodeFrameAsync(session, &bs, nullptr, &surface, &sync)` 由解码器自行分配输出
  surface，同样先 `Map(MFX_MAP_READ)` 再按 pitch 拷出 NV12，然后 `Unmap` + `Release`；
  再转 BGR24 / BGRA32。
- **尺寸规则（在 Intel UHD 770 / Alder Lake-S 上实测）**：

  | 项 | 规则 | 理由 |
  | --- | --- | --- |
  | NV12 表面宽高 | 必须偶数 | 2x2 色度子采样 |
  | NV12 表面高度 | 至少 16 行 | JPEG 编码器按 16 行 MCU 工作，`Height<16` 时 `Init` 返回 `MFX_ERR_INVALID_VIDEO_PARAM`（257x8 失败、257x16 成功） |
  | JPEG 里的 SOF 尺寸 | 等于 `CropW/CropH`，即真实尺寸 | 运行库按 Crop 写 SOF，所以补的是「表面」而不是「图像」 |

  因此编码器把表面宽高向上取偶、高度补到至少 16，而 `CropW/CropH` 保留真实值——
  TIFF 用 32 行 strip 编 193 行的图时，末条只有 1 行，照样得到 SOF=257x1 的合法 JPEG。
  色度按 `ceil(h/2)` 行处理，不足的行按最后一行复制。

## TIFF（Compression=7 / 1）

`Image.Codec.Platform.Intel::Tiff` 自写 TIFF 6.0 容器：

- 每条 strip 调用一次编码器（硬件或软件参考实现，接口统一为 `JpegStripEncoder`），
  高度就是该条实际覆盖的行数；
- 从编码器返回的完整码流里拆出 DQT/DHT 写进 `JPEGTables`(347)，strip 内写**缩写码流**
  （SOI + SOF + SOS + 熵数据 + EOI）；`YCbCrSubSampling`(530) 从 SOF0 的采样因子读回，
  `ReferenceBlackWhite`(532) 按 TIFF 6.0 §21 显式写出；
- 各 strip 的量化/霍夫曼表必须一致，否则直接报错——共用一份 `JPEGTables` 是不容商量
  的前提，写出一个「别的软件打不开」的文件比失败更糟；
- 另有 `Compression=1`（未压缩 RGB）基线，用作像素级对照。

两个容易踩的坑（都有实测依据）：

| 坑 | 后果 |
| --- | --- |
| strip 里不带 SOI | libtiff 解码每条 strip 前会 `jpeg_abort`，再从头读标记；没有 SOI 会被 libjpeg 判成 `Not a JPEG file` |
| 把宽度补到偶数再编码 | libtiff 拿 SOF 宽度与 `ImageWidth` 比对，JPEG 比图像宽就报 `JPEG strip/tile size exceeds expected dimensions` 并**拒绝解码** |
| 把末条 strip 补满 32 行再编码 | 高度方向上 libtiff 只发警告，但会刷屏；所以末条只编码它实际覆盖的行数，而编码器负责把**表面**补到 16 行、SOF 仍写真实高度 |

## 构建与自测

```bash
# 0) 首次克隆：取 oneVPL 头（缺了会 FATAL_ERROR 提示这一句）
git submodule update --init --recursive

# 1) 只构建库（不需要 libjpeg / libtiff，也不需要装 oneVPL 运行库）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# 2) 连自测 CLI 一起构建（libjpeg / libtiff 可选，缺了就 SKIP 对应自测项）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DOIC_BUILD_CLI=ON
cmake --build build -j

# 3) 硬件路径验证：依赖没装全时这里是 SKIP 或明确的失败原因
./build/src/cli/icp_selftest units
./build/src/cli/icp_selftest selftest --require-hardware
```

configure 阶段会把实际找到的依赖打出来（`icp_selftest: libjpeg 3.1.3` /
`未找到 libtiff（容器自测将跳过）`），不需要猜。若 `units` 报「实现数量：0」，
按上面 **依赖清单 → 运行期** 的三件套逐条核对。

```
icp_selftest                                   # = selftest
icp_selftest units                             # 只打印硬件单元
icp_selftest encode <in.bmp> <out.jpg> [q]     # 硬件 JPEG 编码
icp_selftest decode <in.jpg> <out.bmp>         # 硬件 JPEG 解码
icp_selftest tiff <in.bmp> <out.tiff> [--rows N] [--quality Q] [--none] [--software]
icp_selftest selftest [--require-hardware]
```

自测的三种结果，不要混为一谈：

- **PASS**：容器三种形态（缩写+JPEGTables / 完整码流 / 未压缩）都被 libtiff 解开，
  未压缩形态逐字节一致，JPEG 形态 PSNR 达标；
- **SKIP**：没有 Intel 媒体引擎（缺 `vpl-gpu-rt`）或构建时没找到 libjpeg/libtiff；
  这是正常结果，不是失败；
- **FAIL**：`--require-hardware` 明确要求硬件路径可跑，而它跑不了。

`--software` 用 libjpeg 参考编码器产出 strip，因此**没有 Intel 硬件的机器也能完整验证
TIFF 容器**——容器代码与硬件路径不互相掩盖。

## 与父仓库的关系

父仓库 `Image.Codec.Offload` 用 `IMAGECODEC_OFFLOAD_WITH_PLATFORM=ON` 挂载本子项目。
当前只提供库与自测 CLI，**尚未接入路由后端表**：`ico backends` / `ico encode --backend`
里没有它，主项目的 `Image.Codec.Offload.Router` 也还没有 Platform 适配层。
这是刻意的分步交付——先把「硬件能不能用、容器写得对不对」验证清楚，再接路由。

## 目录

```
src/libs/Image.Codec.Platform.Intel.Core/      oic_platform.h + win/posix 实现
src/libs/Image.Codec.Platform.Intel.Jpeg.oneVPL/   vpl_dyn（动态加载）/ vpl_device（单元探测与会话）
                                           vpl_jpeg_encoder / vpl_jpeg_decoder / rgb_nv12 / bmp24
                                            onevpl_test（C 入口）
src/libs/Image.Codec.Platform.Intel.Tiff/          jpeg_stream（SOF0 解析、表拆分）
                                           tiff_jpeg_strip_writer（IFD 写出）
src/cli/                                   icp_selftest + selftest_support
```

## 许可

见仓库根 `LICENSE`。
