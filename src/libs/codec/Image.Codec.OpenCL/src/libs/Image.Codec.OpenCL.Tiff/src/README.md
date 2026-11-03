<!--
 * File: README.md
 * Brief: TIFF 模块.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
-->

# TIFF 模块

- `lzw_codec.*`：统一编解码入口（encode 可选 GPU/CPU；decode 保持 CPU 默认）。
- `lzw_encode_ocl.*` + `lzw_encode.cl` / `lzw_encode_tile.cl`：GPU LZW 编码
  （strip/tile 并行，CL1.2）。
- `lzw_decode_cpu.*`：CPU LZW 解码（唯一正式解码实现）。
- `lzw_decode_ocl.*` + `kernels/lzw_decode.cl`：GPU LZW 解码**探索项**。

## 【探索结论】GPU 并行 LZW 解码（阶段 1.5）

日期：2026-08-12。平台：Intel UHD 770 / OpenCL 3.0 NEO。

- **结论：不建议 GPU 化，CPU `lzwDecode` 保持唯一实现。**
- 原因：LZW 解码的码流解析是硬串行依赖——码宽由段内码序号决定，段边界
  （ClearCode 位置）只能顺序读码发现。段级并行必须先做一次串行段扫描
  （已读全部码），且典型 strip 段数极少（1~3），单段流退化为单 work-item
  串行 kernel，再加两遍（测长+写出）kernel 与设备/缓冲开销，实测不优于 CPU。
- 建议：若未来需要 GPU 解码提速，方向是「按 strip 批处理 + 每 strip 一个
  work-item」（镜像 `lzw_encode_ocl` 架构）+ 两遍(测长+写出) kernel；
  不要在单流接口上追求段内并行。本 `gpuLzwDecode` 保留作对拍/验证基准。

详见 `lzw_decode_ocl.h` 顶部完整记录。
