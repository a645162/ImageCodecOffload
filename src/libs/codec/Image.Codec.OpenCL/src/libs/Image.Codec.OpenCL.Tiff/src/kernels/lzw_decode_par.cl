/*
 * File: lzw_decode_par.cl
 * Brief: Lzw decode par.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// TIFF LZW decoder kernel, SEGMENT-INTERNAL parallel (OpenCL 1.2)。
//
// 核心洞察：LZW 解码在「段内」（无 ClearCode 的连续码字序列）其实可并行：
//   1) 码宽只由段内码序号 k 决定（9/10/11/12 按 k 阈值），与码值无关；
//   2) 因此每码字的 bit 偏移可解析计算（分段线性公式，见 code_bit_offset），
//      每码字一个 work-item 并行读码；
//   3) 字典项 e=257+k（k≥1，第 k 个码产生）的 prefix = 上一个码字 code[k-1]，
//      无依赖、可并行；
//   4) suffix[e] = first_char(dict[code[k]]) = 沿 prefix 链到根的字面量，
//      每 work-item 独立沿 code[] 链并行求（链深度上限 4096）；
//   5) 输出长度 = 链深（同一次沿链求得），前缀和得偏移，并行写出。
//
// 语义与 CPU 参考 lzwDecode（lzw_decode_cpu.cpp）逐字节一致：
//   - 解码器滞后 1：段首码字面量不建表（第 k 个码的字典项号 e=257+k，k=0 无项）；
//   - 码宽时机：段内第 k 个码 9/10/11/12 位于 k 越过 253/765/1789（等价于 CPU
//     参考 next_code==511/1023/2047 切换）；
//   - KwKwK：code[k]==e（e<4096）时输出 dict[old]+first(dict[old])；
//   - 字典满 4096 后停止加表（k≥3839 不建项，next_code 保持 4096）。
//
// 三段 kernel（各为一个 ND-range，全局同步天然满足段间依赖）：
//   lzw_par_extract: 并行读全部码字 + 校验（-3/-4/-5，同 CPU 参考错误码）。
//   lzw_par_build:   并行建字典（suffix[]）+ 求每码字输出长度（同一趟沿链）。
//   lzw_par_write:   并行展开输出（按 offsets 前缀和写入；无需栈，逆序从尾部写）。
//
// 输入约束：host 在 in 尾部填充 ≥3 字节 0（变长码读取需要 3 字节窗口）；
// 字典/码字缓冲按 [n_seg*4096] 布局，每段独立。

#define LZW_FIRST 258u
#define LZW_MAX   4096u

// 段内第 k 个码（0 起）的码宽（同 lzw_decode.cl 的 seg_width）。
inline uint seg_width(uint k)
{
    if (k <= 253u)  return 9u;
    if (k <= 765u)  return 10u;
    if (k <= 1789u) return 11u;
    return 12u;
}

// 段内第 k 个码的 bit 偏移（解析式，等价于对 seg_width 做前缀和）。
// 9 位×254 + 10 位×512 + 11 位×1024 = 2286 + 5120 + 11264 = 18670。
inline uint code_bit_offset(uint seg_start, uint k)
{
    if (k <= 253u)  return seg_start + 9u * k;
    if (k <= 765u)  return seg_start + 2286u + 10u * (k - 254u);
    if (k <= 1789u) return seg_start + 7406u + 11u * (k - 766u);
    return seg_start + 18670u + 12u * (k - 1790u);
}

// 读取 MSB-first 变长码（同 lzw_decode.cl；需 in[byte+2] 可读，host 已填充）。
inline uint read_code(const __global uchar* in, uint bitpos, uint w)
{
    const uint byte = bitpos >> 3u;
    const uint bit  = bitpos & 7u;
    const uint concat = ((uint)in[byte] << 16) | ((uint)in[byte + 1u] << 8) | (uint)in[byte + 2u];
    return (concat >> (24u - bit - w)) & ((1u << w) - 1u);
}

// ---- kernel 1：并行读码 + 校验 ----
__kernel void lzw_par_extract(
    const __global uchar* restrict in,          // 输入码流（尾部填充 3 字节 0）
    const __global uint* restrict seg_start,    // [n_seg] 段起始 bit
    const __global uint* restrict seg_ncodes,   // [n_seg] 段内码字数
    __global uint* restrict codes,              // [n_seg*4096] 提取的码字数组
    __global int* restrict status,              // [n_seg] 0=ok，负=错误码
    const uint n_seg)
{
    const uint s = get_global_id(1);
    if (s >= n_seg) return;
    const uint k = get_global_id(0);
    const uint ncodes = seg_ncodes[s];
    if (k >= ncodes) return;

    const uint off = code_bit_offset(seg_start[s], k);
    const uint code = read_code(in, off, seg_width(k));
    codes[(size_t)s * 4096u + k] = code;

    int st = 0;
    if (code >= LZW_MAX) {
        st = -3;                              // 非法码
    } else if (k == 0u) {
        if (code >= 256u) st = -4;            // 段首必须为字面量
    } else {
        uint nc = 257u + k;                   // 当前 next_code（解码器滞后 1）
        if (nc > LZW_MAX) nc = LZW_MAX;
        if (code > nc) st = -5;               // 码超前
    }
    if (st != 0) status[s] = st;              // 良性竞争：任何非零值都代表失败
}

// ---- kernel 2：并行建字典（suffix）+ 求输出长度 ----
// 字典项 e=257+k（k≥1）：prefix[e]=code[k-1]（old），suffix[e]=first_char(dict[code[k]])。
// 同一趟沿 code[] 链走到根字面量：根 = first char（写 suffix[e]），步数 = 串长（写 lengths[k]）。
__kernel void lzw_par_build(
    const __global uint* restrict codes,       // [n_seg*4096]
    __global uchar* restrict suffix,           // [n_seg*4096]
    __global uint* restrict lengths,           // [n_seg*4096] 每码字输出字节数
    const __global uint* restrict seg_ncodes,  // [n_seg]
    const uint n_seg)
{
    const uint s = get_global_id(1);
    if (s >= n_seg) return;
    const uint k = get_global_id(0);
    const uint ncodes = seg_ncodes[s];
    if (k >= ncodes) return;

    const __global uint* cd = codes + (size_t)s * 4096u;
    __global uchar* sf = suffix + (size_t)s * 4096u;
    __global uint* ln = lengths + (size_t)s * 4096u;

    if (k == 0u) { ln[0] = 1u; return; }      // 段首字面量：1 字节

    const uint c = cd[k];
    const uint e = 257u + k;
    uint node, depth;
    if (c == e) { node = cd[k - 1u]; depth = 1u; }  // KwKwK：dict[old]+first(dict[old])
    else        { node = c;         depth = 0u; }

    while (node >= LZW_FIRST) {
        depth++;
        node = cd[node - 258u];               // prefix[node] = code[node-258]
    }
    if (e < LZW_MAX) sf[e] = (uchar)node;     // suffix[e] = first char（根字面量）
    ln[k] = depth + 1u;                       // +1 为根字面量自身
}

// ---- kernel 3：并行展开输出 ----
// 从串尾逆序写：先写各节点 suffix（链尾在前），最后写根字面量；KwKwK 末尾补根字节。
__kernel void lzw_par_write(
    const __global uint* restrict codes,       // [n_seg*4096]
    const __global uchar* restrict suffix,     // [n_seg*4096]
    const __global uint* restrict lengths,     // [n_seg*4096]
    const __global uint* restrict offsets,     // [n_seg*4096] 每码字在段内输出偏移
    const __global uint* restrict out_base,    // [n_seg] 段输出在全局输出的偏移
    __global uchar* restrict out,              // 输出
    const __global uint* restrict seg_ncodes,  // [n_seg]
    const uint n_seg)
{
    const uint s = get_global_id(1);
    if (s >= n_seg) return;
    const uint k = get_global_id(0);
    const uint ncodes = seg_ncodes[s];
    if (k >= ncodes) return;

    const __global uint* cd = codes + (size_t)s * 4096u;
    const __global uchar* sf = suffix + (size_t)s * 4096u;
    const uint base = out_base[s];
    const uint off = offsets[(size_t)s * 4096u + k];
    const uint len = lengths[(size_t)s * 4096u + k];

    if (k == 0u) { out[base + off] = (uchar)cd[0]; return; }

    const uint c = cd[k];
    const uint e = 257u + k;
    const int kw = (c == e) ? 1 : 0;
    uint node = kw ? cd[k - 1u] : c;
    uint p = kw ? (len - 1u) : len;
    while (node >= LZW_FIRST) {
        out[base + off + (--p)] = sf[node];
        node = cd[node - 258u];
    }
    out[base + off + (--p)] = (uchar)node;
    if (kw) out[base + off + (len - 1u)] = (uchar)node;
}
