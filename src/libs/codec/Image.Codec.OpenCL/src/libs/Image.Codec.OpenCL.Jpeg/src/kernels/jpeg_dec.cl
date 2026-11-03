/*
 * File: jpeg_dec.cl
 * Brief: Jpeg dec.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/kernels/jpeg_dec.cl - 解码 GPU kernels：
//   1) dequant_idct : 反量化 + 2D IDCT（逐 8x8 块，输出整数化 0..255 float 平面）
//   2) ycbcr_to_rgb : YCbCr->RGB + 色度上采样（4:2:0 h2v2 / 4:2:2 h2v1，libjpeg 三角滤波）
// 依赖宿主预计算的 64x64 DCT 基矩阵 idct_mat（含 1/4 归一化与 C(u)C(v)）。
// 编译选项 -cl-std=CL1.2。

// 读分量平面样本（clamp 到有效区域，返回整数样本）。
inline int rd(__global const float* base, int stride, int pw, int ph, int cx,
              int cy) {
  if (cx < 0) cx = 0;
  if (cx >= pw) cx = pw - 1;
  if (cy < 0) cy = 0;
  if (cy >= ph) cy = ph - 1;
  return (int)base[cy * stride + cx];
}

// h2v2 fancy 上采样（libjpeg jdsample.c h2v2_fancy_upsample 逐像素推导）。
inline int upsample_h2v2(__global const float* base, int st, int pw, int ph,
                         int x, int y) {
  int cx = x >> 1;
  int cy = y >> 1;
  int h = x & 1;
  int v = y & 1;
  if (cx > pw - 1) cx = pw - 1;
  if (cy > ph - 1) cy = ph - 1;
  int iy = cy;
  int oy2 = (v == 0) ? (cy > 0 ? cy - 1 : cy) : (cy + 1 < ph ? cy + 1 : cy);
  int s_cur = 3 * rd(base, st, pw, ph, cx, iy) + rd(base, st, pw, ph, cx, oy2);
  int out;
  if (h == 0) {
    if (cx == 0) {
      out = (4 * s_cur + 8) >> 4;
    } else {
      out = (3 * s_cur + 3 * rd(base, st, pw, ph, cx - 1, iy) +
             rd(base, st, pw, ph, cx - 1, oy2) + 8) >>
            4;
    }
  } else {
    if (cx >= pw - 1) {
      out = (4 * s_cur + 7) >> 4;
    } else {
      out = (3 * s_cur + 3 * rd(base, st, pw, ph, cx + 1, iy) +
             rd(base, st, pw, ph, cx + 1, oy2) + 7) >>
            4;
    }
  }
  return out;
}

// h2v1 fancy 上采样（libjpeg h2v1_fancy_upsample 逐像素推导）。
inline int upsample_h2v1(__global const float* base, int st, int pw, int ph,
                         int x, int y) {
  int cx = x >> 1;
  int h = x & 1;
  if (cx > pw - 1) cx = pw - 1;
  int cur = rd(base, st, pw, ph, cx, y);
  if (h == 0) {
    if (cx == 0) return cur;
    return (3 * cur + rd(base, st, pw, ph, cx - 1, y) + 1) >> 2;
  } else {
    if (cx >= pw - 1) return cur;
    return (3 * cur + rd(base, st, pw, ph, cx + 1, y) + 2) >> 2;
  }
}

// 按分量 c 取 (x,y) 处的样本（含上采样）。
inline int sample_c(__global const float* planes, __constant int* desc, int c,
                    int max_h, int max_v, int x, int y) {
  int off = desc[c * 6 + 0];
  int st = desc[c * 6 + 1];
  int pw = desc[c * 6 + 2];
  int ph = desc[c * 6 + 3];
  int hs = desc[c * 6 + 4];
  int vs = desc[c * 6 + 5];
  __global const float* base = planes + off;
  if (hs == max_h && vs == max_v) {
    return rd(base, st, pw, ph, x, y);
  }
  int hratio = max_h / hs;
  int vratio = max_v / vs;
  if (hratio == 2 && vratio == 2) {
    return upsample_h2v2(base, st, pw, ph, x, y);
  }
  if (hratio == 2 && vratio == 1) {
    return upsample_h2v1(base, st, pw, ph, x, y);
  }
  return rd(base, st, pw, ph, x * hs / max_h, y * vs / max_v);
}

// 反量化 + 2D IDCT。每个 work-item 计算一个块的 64 个输出像素之一。
//   coeffs[coeff_off + bid*64 + k] : 量化系数（自然序）
//   qbuf[tq*64 + k]                : 量化表（自然序）
//   idct_mat[p*64 + k]             : IDCT 基（p = 输出像素，k = 系数）
__kernel void dequant_idct(__global const float* coeffs, int coeff_off,
                           __global const float* qbuf,
                           __global const float* idct_mat,
                           __global float* planes, int plane_off,
                           int plane_stride, int block_stride, int blocks_total,
                           int qtable_index) {
  uint gid = get_global_id(0);
  uint bid = gid >> 6;
  uint p = gid & 63;
  if (bid >= (uint)blocks_total) return;
  const __global float* c = coeffs + coeff_off + (bid << 6);
  const __global float* q = qbuf + qtable_index * 64;
  // IDCT 基矩阵按 [系数][像素] 存放，此处按像素 p 沿列求和。
  const __global float* m = idct_mat + p;
  float sum = 0.0f;
  for (int k = 0; k < 64; k++) {
    sum += m[k * 64] * c[k] * q[k];
  }
  // 解码端 IDCT 输出为 [-128,127] 电平，加 128 还原样本值。
  int iv = (int)floor(sum + 128.0f + 0.5f);
  if (iv < 0) iv = 0;
  if (iv > 255) iv = 255;
  int bx = p & 7;
  int by = p >> 3;
  int gx = (bid % (uint)block_stride) * 8 + bx;
  int gy = (bid / (uint)block_stride) * 8 + by;
  planes[plane_off + gy * plane_stride + gx] = (float)iv;
}

// YCbCr -> RGB（libjpeg 整数系数：91881/46802/22554/116130, >>16 舍入）
//  + 色度上采样（4:2:0/4:2:2）。输出 RGB 行序（top-down），宽度 width。
//   desc[3*6]：每分量 {off, stride, plane_w, plane_h, h_samp, v_samp}
__kernel void ycbcr_to_rgb(__global const float* planes, __constant int* desc,
                           int max_h, int max_v, int ncomp,
                           __global uchar* out, int out_stride, int width,
                           int height, int out_channels) {
  int x = get_global_id(0);
  int y = get_global_id(1);
  if (x >= width || y >= height) return;
  int yv = sample_c(planes, desc, 0, max_h, max_v, x, y);
  int r, g, b;
  if (ncomp >= 3) {
    int cb = sample_c(planes, desc, 1, max_h, max_v, x, y);
    int cr = sample_c(planes, desc, 2, max_h, max_v, x, y);
    int cbd = cb - 128;
    int crd = cr - 128;
    r = yv + ((91881 * crd + 32768) >> 16);
    g = yv - ((46802 * crd + 32768) >> 16) - ((22554 * cbd + 32768) >> 16);
    b = yv + ((116130 * cbd + 32768) >> 16);
  } else {
    r = g = b = yv;
  }
  if (r < 0) r = 0;
  if (r > 255) r = 255;
  if (g < 0) g = 0;
  if (g > 255) g = 255;
  if (b < 0) b = 0;
  if (b > 255) b = 255;
  // 输出通道数与输入分量数一致：单分量 JPEG 就写 1 字节/像素，
  // 绝不在这里把灰度复制成 RGB（通道保真）。
  int o = y * out_stride + x * out_channels;
  if (out_channels == 1) {
    out[o] = (uchar)yv;
    return;
  }
  out[o + 0] = (uchar)r;
  out[o + 1] = (uchar)g;
  out[o + 2] = (uchar)b;
}
