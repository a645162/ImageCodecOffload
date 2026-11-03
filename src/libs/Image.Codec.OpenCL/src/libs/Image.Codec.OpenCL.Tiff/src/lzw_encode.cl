/*
 * File: lzw_encode.cl
 * Brief: Lzw encode.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// TIFF LZW encoder kernel, strip-parallel (OpenCL 1.2)。
// 每个 work-item 独立编码一条 strip，使用全局内存中的开放寻址哈希字典。
// 字典槽位打包为 uint：((key+1)<<12) | code（一次探测一次内存访问）。
//
// 码宽增长 9->10->11->12 于 next_code == 512/1024/2048 时切换，
// 满字典(next_code==4096)发 ClearCode(256) 并重置字典，EOI(257) 结束。
// 该宽度时机与 libtiff 编码器一致，也与本库 CPU 解码器(lzw_decode_cpu)一致。

inline uint lzw_hash(uint key)
{
    return (uint)((key * 2654435761u) >> 20) & 4095u;
}

inline void put_bits(__global uchar* out, uint* out_len, uint* bitbuf, int* nbits,
                     uint value, int width)
{
    *bitbuf = (*bitbuf << width) | value;
    *nbits += width;
    while (*nbits >= 8) {
        *nbits -= 8;
        out[(*out_len)++] = (uchar)(*bitbuf >> *nbits);
    }
    *bitbuf &= (1u << *nbits) - 1u;
}

__kernel void lzw_encode(
    const __global uchar* restrict input,   // [n_strips * strip_bytes]
    __global uint* restrict dict,           // [n_strips * 4096] packed ((key+1)<<12)|code
    __global uchar* restrict output,        // [n_strips * max_strip]
    __global uint* restrict strip_sizes,    // [n_strips]
    const uint strip_bytes,                 // RowsPerStrip * row_bytes
    const uint max_strip,
    const uint n_strips)
{
    const uint s = get_global_id(0);
    if (s >= n_strips) return;
    const __global uchar* in  = input  + (size_t)s * strip_bytes;
    __global uchar* out       = output + (size_t)s * max_strip;
    __global uint* d          = dict   + (size_t)s * 4096;

    uint bitbuf = 0;
    int nbits = 0;
    uint out_len = 0;
    uint next_code = 258;
    int width = 9;

    put_bits(out, &out_len, &bitbuf, &nbits, 256u, 9); // ClearCode

    uint prefix = (uint)in[0];
    for (uint i = 1; i < strip_bytes; i++) {
        const uint byte = (uint)in[i];
        const uint key  = (prefix << 8) | byte;
        uint h = lzw_hash(key);
        uint e = d[h];
        while (e != 0 && (e >> 12) != key + 1) { h = (h + 1) & 4095u; e = d[h]; }
        if ((e >> 12) == key + 1) {
            prefix = e & 4095u;
        } else {
            put_bits(out, &out_len, &bitbuf, &nbits, prefix, width);
            if (next_code < 4096) {
                d[h] = ((key + 1) << 12) | next_code;
                next_code++;
                if (next_code == 512)       width = 10;
                else if (next_code == 1024) width = 11;
                else if (next_code == 2048) width = 12;
            } else {
                put_bits(out, &out_len, &bitbuf, &nbits, 256u, width);
                for (uint j = 0; j < 4096; j++) { d[j] = 0; }
                next_code = 258;
                width = 9;
            }
            prefix = byte;
        }
    }
    put_bits(out, &out_len, &bitbuf, &nbits, prefix, width);
    // 最后一个数据码之后补一次「假想建表」的宽度记账：解码器读到最后一个码
    // 仍会建一条（下一条才用得到的）条目，若这次建表正好跨过 512/1024/2048，
    // 它会按更宽的码读 EOI；编码器不补这一步就会位流错位。
    if (next_code < 4096u) {
      next_code++;
      if (next_code == 512u) width = 10;
      else if (next_code == 1024u) width = 11;
      else if (next_code == 2048u) width = 12;
    }
    put_bits(out, &out_len, &bitbuf, &nbits, 257u, width); // EOI
    if (nbits > 0) {
        out[out_len++] = (uchar)(bitbuf << (8 - nbits));
    }
    strip_sizes[s] = out_len;
}
