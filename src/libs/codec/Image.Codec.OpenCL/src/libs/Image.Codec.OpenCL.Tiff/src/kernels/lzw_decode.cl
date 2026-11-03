/*
 * File: lzw_decode.cl
 * Brief: Lzw decode.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// TIFF LZW decoder kernel, SEGMENT-parallel (OpenCL 1.2)。
// 每个 work-item 独立解码一个「段」（段 = 相邻两次 ClearCode/EOI 之间的码序列，
// 段内字典独立，可从 ClearCode 之后的字面量重新开始，故可并行）。
//
// 段内解码逻辑与 CPU 参考实现 lzwDecode（lzw_decode_cpu.cpp）逐字节一致：
//   - 码流 MSB-first、变长码；码宽由「段内码序号」决定（位置公式，等价于
//     CPU 参考中 next_code==511/1023/2047 时切换，见下 seg_width 说明）。
//   - ClearCode(256) 之后第一个码必须为字面量(<256)；KwKwK(code==next_code)
//     分支；字典满(next_code==4096)后停止加表。
//
// 两遍调用（同一 kernel，mode 区分）：
//   mode==0 只统计各段输出长度到 seg_len（不写 out）；
//   mode==1 把各段输出写入 out[seg_out_offset[s] ..]（按前面统计的偏移）。
// 两遍产生逐字节一致的结果，避免为输出长度做宽松上界分配。
//
// 输入约束：host 在 in 尾部填充 ≥3 字节 0（变长码读取需要 3 字节窗口）。

#define LZW_CLEAR 256u
#define LZW_EOI   257u
#define LZW_FIRST 258u
#define LZW_MAX   4096u
#define LZW_NOPREF 0xFFFFu

// 段内第 j 个码（0 起）的码宽。推导：段内每码（首字面量除外）加一个字典条目，
// 解码器于 next_code==511/1023/2047 后切换（比编码器早一位，见 CPU 参考注释）。
// 于是 0..253 → 9 位，254..765 → 10 位，766..1789 → 11 位，1790+ → 12 位。
inline uint seg_width(uint j)
{
    if (j <= 253u) return 9u;
    if (j <= 765u) return 10u;
    if (j <= 1789u) return 11u;
    return 12u;
}

// 读取 MSB-first 变长码：bit 0 为 in[0] 的最高位。需要 in[byte+2] 可读（host 已填充）。
inline uint read_code(const __global uchar* in, uint bitpos, uint w)
{
    const uint byte = bitpos >> 3u;
    const uint bit  = bitpos & 7u;
    const uint concat = ((uint)in[byte] << 16) | ((uint)in[byte + 1u] << 8) | (uint)in[byte + 2u];
    return (concat >> (24u - bit - w)) & ((1u << w) - 1u);
}

__kernel void lzw_decode(
    const __global uchar* restrict in,          // 输入码流（尾部填充 3 字节 0）
    __global ushort* restrict prefix,           // [n_seg*4096] 前缀码（0xFFFF=无）
    __global uchar* restrict suffix,            // [n_seg*4096] 后缀字节
    __global uchar* restrict stack,             // [n_seg*4128] 前缀链展开栈
    __global uchar* restrict out,               // 输出（mode==1 用）
    const __global uint* restrict seg_start,    // [n_seg] 段起始 bit
    const __global uint* restrict seg_end,      // [n_seg] 段结束 bit（不含终止码）
    const __global uint* restrict seg_out_offset, // [n_seg] 输出字节偏移（mode==1 用）
    __global uint* restrict seg_len,            // [n_seg] 输出长度（mode==0 写）
    __global int* restrict seg_status,          // [n_seg] 0=ok，负=错误码（同 CPU 参考）
    const uint n_seg,
    const int mode)
{
    const uint s = get_global_id(0);
    if (s >= n_seg) return;

    const uint start = seg_start[s];
    const uint end   = seg_end[s];
    __global ushort* pref = prefix + (size_t)s * 4096u;
    __global uchar*  suff = suffix + (size_t)s * 4096u;
    __global uchar*  st   = stack  + (size_t)s * 4128u;

    // 0..255 字面量条目（prefix=无，suffix=自身）。
    for (uint i = 0u; i < 256u; i++) {
        pref[i] = LZW_NOPREF;
        suff[i] = (uchar)i;
    }

    uint bitpos = start;
    uint next_code = LZW_FIRST;
    int  old = -1;
    uint out_len = 0;
    uint j = 0;                 // 段内码序号
    const uint base = (mode == 0) ? 0u : seg_out_offset[s];
    int status = 0;

    while (bitpos < end) {
        const uint w = seg_width(j);
        const uint code = read_code(in, bitpos, w);
        bitpos += w;
        j++;

        if (code == LZW_EOI)   break;   // 终止码已在 seg_end 之外，此处仅防御
        if (code == LZW_CLEAR) break;   // 同上
        if (code >= LZW_MAX) { status = -3; break; }

        if (old == -1) {
            // ClearCode 之后第一个码必须是字面量。
            if (code >= 256u) { status = -4; break; }
            if (mode == 0) {
                out_len++;
            } else {
                out[base + out_len++] = (uchar)code;
            }
            old = (int)code;
            continue;
        }

        uint sp = 0;
        if (code < next_code) {
            int c = (int)code;
            while (pref[c] != LZW_NOPREF) {
                st[sp++] = suff[c];
                c = (int)pref[c];
            }
            st[sp++] = suff[c];
            const uchar first_byte = st[sp - 1u];
            // 反转输出 dict[code]
            if (mode == 0) {
                out_len += sp;
            } else {
                for (uint i = sp; i-- > 0u;) out[base + out_len++] = st[i];
            }
            // 新条目 = dict[old] + first(dict[code])
            if (next_code < LZW_MAX) {
                pref[next_code] = (ushort)old;
                suff[next_code] = first_byte;
                next_code++;
            }
        } else if (code == next_code) {
            // KwKwK：新条目 = dict[old] + first(dict[old])；输出同新条目。
            int c = old;
            while (pref[c] != LZW_NOPREF) {
                st[sp++] = suff[c];
                c = (int)pref[c];
            }
            st[sp++] = suff[c];
            const uchar first_byte = st[sp - 1u];
            if (mode == 0) {
                out_len += sp;
            } else {
                for (uint i = sp; i-- > 0u;) out[base + out_len++] = st[i];
            }
            if (mode == 0) {
                out_len++;
            } else {
                out[base + out_len++] = first_byte;
            }
            if (next_code < LZW_MAX) {
                pref[next_code] = (ushort)old;
                suff[next_code] = first_byte;
                next_code++;
            }
        } else {
            status = -5;   // 非法码（code > next_code）
            break;
        }
        old = (int)code;
    }

    if (mode == 0) seg_len[s] = out_len;
    seg_status[s] = status;
}
