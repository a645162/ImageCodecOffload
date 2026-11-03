/*
 * File: lzw_decode_ocl.h
 * Brief: Lzw decode ocl.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/lzw_decode_ocl.h - GPU LZW 解码（探索项，阶段 1.5）。
//
// ============================================================================
// 【探索结论】GPU 并行 LZW 解码 —— 两个变体：
//   gpuLzwDecode（段级并行，段内串行）与 gpuLzwDecodePar（段内并行）。
//   段内并行变体在大数据上显著快于 CPU，小数据因固定开销劣于 CPU。
// ============================================================================
// 结论日期：2026-08-12。平台：Intel UHD 770 / OpenCL 3.0 NEO / CPU 参考 lzwDecode。
//
// 变体 1 —— gpuLzwDecode()：段级并行。把码流按 ClearCode(256) 切段（段间字典独立、
// 可并行），段内仍串行。host 先做一次「段扫描」找段边界并校验码流，再启动 kernel
// （每 work-item 解码一段），两遍（测长+写出）。实测劣于 CPU：单段流退化为 1 个
// work-item 的纯串行 kernel，加上两遍内核调用与设备/缓冲开销，净效果反而慢。
//
// 变体 2 —— gpuLzwDecodePar()：段内并行（本次新增，见 kernels/lzw_decode_par.cl）。
//   关键洞察：LZW 解码在「段内」其实可并行——码宽只由段内码序号 k 决定，与码值无关，
//   因此每码字的 bit 偏移可解析计算（分段线性公式），全部码字并行读；字典项
//   e=257+k 的 prefix 无依赖并行得（=code[k-1]），suffix 沿 code[] 链每 work-item
//   并行求根字面量；输出长度=链深（同一趟沿链求得），前缀和定位后并行展开。
//   流程：host 段扫描（找段边界，串行）-> 3 个 kernel（读码校验 / 建字典+测长 /
//   写输出，2D ND-range = 段 x 码字），多段通过 2D ND-range 一并并行处理。
//
// 对拍结果：与 CPU 参考逐字节一致（含 off-by-one 宽度时机、KwKwK、段首字面量、
// 字典满 4096 无 ClearCode 的 >3838 码段、坏流错误码 -2/-4/-5）。空流、128 个
// rps=1 小 strip、1MB 合成数据、1024x1024 单 strip 均逐字节一致。
//
// 性能（同一输入，含设备初始化/编译/kernel/传输的完整调用）：
//   - 单段 3700 码字（5KB 压缩）：CPU 0.18ms vs par 1.44ms（约 8 倍慢，固定开销主导）
//   - 128x128 图像（46KB 压缩）：CPU 2.25ms vs par 2.25ms（持平）
//   - 平色 49152B（深链+KwKwK）：CPU 1.94ms vs par 1.56ms（约 1.2 倍快）
//   - 半随机 1MB（约 260 段）：CPU 49.7ms vs par 24.9ms（约 2 倍快）
//   - 1024x1024 图像单 strip（1.6MB 压缩 / 3MB 输出）：CPU 136.8ms vs par 29.6ms
//     （约 4.6 倍快；par 纯解码约 29.2ms）
//   相比段级并行 gpuLzwDecode 快约 4~12 倍。固定开销（设备初始化 0.12ms + kernel
//   编译 0.31ms）约 0.43ms/次调用，是小型输入（<~10KB 压缩）上慢于 CPU 的主因。
//
// 建议：
//   - 段内并行思路有效，但单流接口每次调用的固定开销（设备初始化 + 编译 + 段扫描 +
//     长度回读/偏移写回）使小数据不划算。若接入实际解码路径：
//     a) 持久化 OclDevice/OclProgram（初始化一次复用），消除 0.43ms 固定开销；
//     b) 按 strip 批处理（一次调用解多条 strip，2D ND-range 天然支持）；
//     c) 大数据（≥~50KB 压缩块）直接用 gpuLzwDecodePar，小数据回退 CPU。
//   - 本文件保留 gpuLzwDecode（段级并行）与 gpuLzwDecodePar（段内并行）供探索/对拍。
//
// 遗留风险：
//   - 段扫描与 kernel 的宽度时机（解码器 511/1023/2047 vs 编码器 512/1024/2048）
//     高度微妙，改动需同步 CPU 参考与 GPU 编码 kernel 三处。
//   - 本实现仅处理 ≤2^28 字节的单流；超大流可回退 CPU。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace image_codec {
namespace tiff {

// 用 GPU（段级并行）解码一段 LZW 压缩数据，语义与 CPU 参考 lzwDecode 一致
// （ClearCode/EOI、9->10->11->12 码宽、字典满 4096 重置、KwKwK 分支）。
// 成功返回 0，out 为全部解压字节；失败返回负值：
//   -1 输入非法；-2 位流提前结束（缺 EOI）；-3 非法码；-4 非字面量；
//   -5 码超前；-10.. 设备/编译/运行错误。
// 任何负值返回都表示失败，调用方应回退 CPU lzwDecode。
int gpuLzwDecode(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out);

// 用 GPU（段内并行）解码一段 LZW 压缩数据。语义与 CPU 参考 lzwDecode 一致。
// 与 gpuLzwDecode（段级并行，段内串行）的不同：段内每个码字一个 work-item——
//   1) 码宽只由段内码序号决定，bit 偏移可解析计算，全部码字并行读；
//   2) 字典 prefix 无依赖并行得，suffix 沿 code[] 链每 work-item 并行求；
//   3) 输出长度并行求（前缀和定位），输出并行展开。
// 多段（字典满）通过 2D ND-range（段 x 码字）逐段并行处理。
// 成功返回 0，out 为全部解压字节；失败返回负值（错误码含义同 gpuLzwDecode）。
int gpuLzwDecodePar(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out);

}  // namespace tiff
}  // namespace image_codec
