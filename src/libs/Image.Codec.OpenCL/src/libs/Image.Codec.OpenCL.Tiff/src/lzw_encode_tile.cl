/*
 * File: lzw_encode_tile.cl
 * Brief: Lzw encode tile.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// TIFF LZW encoder kernel, TILE-parallel (OpenCL 1.2)。
// 每个 work-item 独立编码一个 tile（TILE_H x TILE_W 像素，TILE_W == TILE_H == -DTILE_H）。
// 图像由 host 预先填充到 tile 尺寸的整数倍；tile 像素按行主序消耗
// （row0..row0+TILE_H-1, col0..col0+TILE_W*3-1）。
// 码宽/字典行为与 strip 版 kernel / CPU 解码器一致。

inline uint lzw_hash_tile(uint key)
{
    return (uint)((key * 2654435761u) >> 20) & 4095u;
}

inline void put_bits_tile(__global uchar* out, uint* out_len, uint* bitbuf, int* nbits,
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

__kernel void lzw_encode_tile(
    const __global uchar* restrict input,   // padded 图像，每行 padded_row_bytes
    __global uint* restrict dict,           // [block_cap * 4096] packed
    __global uchar* restrict output,        // [block_cap * max_tile]
    __global uint* restrict tile_sizes,     // [block_cap]
    const uint padded_row_bytes,            // padded_W * 3
    const uint tile_px_bytes,               // TILE_W * 3
    const uint tiles_per_row,
    const uint n_tiles,
    const uint max_tile,
    const uint tile_base)                   // 本 block 的第一个全局 tile 下标
{
    const uint lt = get_global_id(0);           // block 内下标（out/dict/sizes）
    const uint t  = lt + tile_base;             // 全局 tile 下标（input 坐标）
    if (t >= n_tiles) return;
    const uint tc = t % tiles_per_row;
    const uint tr = t / tiles_per_row;
    const uint col0 = tc * tile_px_bytes;
    const uint row0 = tr * (uint)TILE_H;
    __global uchar* out = output + (size_t)lt * max_tile;
    __global uint* d   = dict   + (size_t)lt * 4096;

    uint bitbuf = 0;
    int nbits = 0;
    uint out_len = 0;
    uint next_code = 258;
    int width = 9;

    put_bits_tile(out, &out_len, &bitbuf, &nbits, 256u, 9); // ClearCode

    uint prefix = input[(size_t)row0 * padded_row_bytes + col0];
    for (uint r = 0; r < (uint)TILE_H; r++) {
        const __global uchar* line = input + (size_t)(row0 + r) * padded_row_bytes + col0;
        for (uint c = 0; c < tile_px_bytes; c++) {
            if (r == 0 && c == 0) continue;
            const uint byte = (uint)line[c];
            const uint key  = (prefix << 8) | byte;
            uint h = lzw_hash_tile(key);
            uint e = d[h];
            while (e != 0 && (e >> 12) != key + 1) { h = (h + 1) & 4095u; e = d[h]; }
            if ((e >> 12) == key + 1) {
                prefix = e & 4095u;
            } else {
                put_bits_tile(out, &out_len, &bitbuf, &nbits, prefix, width);
                if (next_code < 4096) {
                    d[h] = ((key + 1) << 12) | next_code;
                    next_code++;
                    if (next_code == 512)       width = 10;
                    else if (next_code == 1024) width = 11;
                    else if (next_code == 2048) width = 12;
                } else {
                    put_bits_tile(out, &out_len, &bitbuf, &nbits, 256u, width);
                    for (uint j = 0; j < 4096; j++) { d[j] = 0; }
                    next_code = 258;
                    width = 9;
                }
                prefix = byte;
            }
        }
    }
    put_bits_tile(out, &out_len, &bitbuf, &nbits, prefix, width);
    // 最后一个数据码之后补一次「假想建表」的宽度记账：解码器读到最后一个码
    // 仍会建一条（下一条才用得到的）条目，若这次建表正好跨过 512/1024/2048，
    // 它会按更宽的码读 EOI；编码器不补这一步就会位流错位。
    if (next_code < 4096u) {
      next_code++;
      if (next_code == 512u) width = 10;
      else if (next_code == 1024u) width = 11;
      else if (next_code == 2048u) width = 12;
    }
    put_bits_tile(out, &out_len, &bitbuf, &nbits, 257u, width); // EOI
    if (nbits > 0) {
        out[out_len++] = (uchar)(bitbuf << (8 - nbits));
    }
    tile_sizes[lt] = out_len;
}
