/*
 * File: png_filter.cl
 * Brief: Png filter.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// kernels/png_filter.cl - PNG 行滤波（OpenCL）。
//
// 关键性质：PNG 的 filter 预测值只来自「原始行」的左侧 / 上方 / 左上样本，
// 不依赖同一行已过滤的输出，因此每一行、每一字节都可以完全并行。
// 这正是 PNG 编码里唯一值得放到 GPU 的阶段；deflate 留在 zlib。
//
// 两个 kernel 与 CUDA 后端一一对应，结果必须逐位一致：
//   png_filter_rows : 按给定 filter 生成 filtered 行流
//   png_filter_sums : 为每行统计 5 种 filter 的残差绝对值和（自适应选择用）

inline int png_paeth(int a, int b, int c) {
  int p = a + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

// 单行滤波：全局 (0) = 行内字节下标，(1) = 行号。
// types 为每行 filter 类型数组；uniform_type >= 0 时忽略 types。
__kernel void png_filter_rows(__global const uchar* src,
                              __global uchar* dst,
                              __global const uchar* types, int width,
                              int channels, int uniform_type) {
  int y = get_global_id(1);
  int i = get_global_id(0);
  int row_bytes = width * channels;
  if (i >= row_bytes) return;

  int bpp = channels;
  ulong row_base = (ulong)y * (ulong)row_bytes;
  __global const uchar* row = src + row_base;
  __global const uchar* prev = (y > 0) ? (row - row_bytes) : 0;
  __global uchar* out = dst + (ulong)y * (ulong)(row_bytes + 1);

  int filter = (uniform_type >= 0) ? uniform_type : (int)types[y];
  if (i == 0) out[0] = (uchar)filter;

  int left = (i >= bpp) ? row[i - bpp] : 0;
  int up = (prev != 0) ? prev[i] : 0;
  int ul = (i >= bpp && prev != 0) ? prev[i - bpp] : 0;
  int pred = 0;
  switch (filter) {
    case 1:  // Sub
      pred = left;
      break;
    case 2:  // Up
      pred = up;
      break;
    case 3:  // Average
      pred = (left + up) >> 1;
      break;
    case 4:  // Paeth
      pred = png_paeth(left, up, ul);
      break;
    default:  // None
      pred = 0;
      break;
  }
  out[1 + i] = (uchar)(row[i] - pred);
}

// 自适应滤波统计：一个 work-group 负责一行，组内归约后写 sums[y*5 + f]。
// local 尺寸由宿主裁剪到 [32, 256]，因此下面的 __local 数组固定 256 项足够。
#define PNG_SUMS_MAX_LOCAL 256

__kernel void png_filter_sums(__global const uchar* src, __global ulong* sums,
                              int width, int channels) {
  int y = (int)get_group_id(0);
  int lid = (int)get_local_id(0);
  int lsize = (int)get_local_size(0);
  int row_bytes = width * channels;
  int bpp = channels;

  ulong row_base = (ulong)y * (ulong)row_bytes;
  __global const uchar* row = src + row_base;
  __global const uchar* prev = (y > 0) ? (row - row_bytes) : 0;

  ulong acc0 = 0, acc1 = 0, acc2 = 0, acc3 = 0, acc4 = 0;
  for (int i = lid; i < row_bytes; i += lsize) {
    int v = (int)row[i];
    int left = (i >= bpp) ? row[i - bpp] : 0;
    int up = (prev != 0) ? prev[i] : 0;
    int ul = (i >= bpp && prev != 0) ? prev[i - bpp] : 0;

    int r0 = v;
    int r1 = v - left;
    int r2 = v - up;
    int r3 = v - ((left + up) >> 1);
    int r4 = v - png_paeth(left, up, ul);

    acc0 += (ulong)(r0 < 0 ? -r0 : r0);
    acc1 += (ulong)(r1 < 0 ? -r1 : r1);
    acc2 += (ulong)(r2 < 0 ? -r2 : r2);
    acc3 += (ulong)(r3 < 0 ? -r3 : r3);
    acc4 += (ulong)(r4 < 0 ? -r4 : r4);
  }

  __local ulong lsum[5 * PNG_SUMS_MAX_LOCAL];
  lsum[0 * PNG_SUMS_MAX_LOCAL + lid] = acc0;
  lsum[1 * PNG_SUMS_MAX_LOCAL + lid] = acc1;
  lsum[2 * PNG_SUMS_MAX_LOCAL + lid] = acc2;
  lsum[3 * PNG_SUMS_MAX_LOCAL + lid] = acc3;
  lsum[4 * PNG_SUMS_MAX_LOCAL + lid] = acc4;
  barrier(CLK_LOCAL_MEM_FENCE);

  for (int s = lsize >> 1; s > 0; s >>= 1) {
    if (lid < s) {
      lsum[0 * PNG_SUMS_MAX_LOCAL + lid] += lsum[0 * PNG_SUMS_MAX_LOCAL + lid + s];
      lsum[1 * PNG_SUMS_MAX_LOCAL + lid] += lsum[1 * PNG_SUMS_MAX_LOCAL + lid + s];
      lsum[2 * PNG_SUMS_MAX_LOCAL + lid] += lsum[2 * PNG_SUMS_MAX_LOCAL + lid + s];
      lsum[3 * PNG_SUMS_MAX_LOCAL + lid] += lsum[3 * PNG_SUMS_MAX_LOCAL + lid + s];
      lsum[4 * PNG_SUMS_MAX_LOCAL + lid] += lsum[4 * PNG_SUMS_MAX_LOCAL + lid + s];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if (lid < 5) {
    sums[(ulong)y * 5 + lid] = lsum[lid * PNG_SUMS_MAX_LOCAL];
  }
}
