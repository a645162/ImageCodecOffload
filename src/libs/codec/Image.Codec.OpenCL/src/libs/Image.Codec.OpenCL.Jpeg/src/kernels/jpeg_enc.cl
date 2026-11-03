/*
 * File: jpeg_enc.cl
 * Brief: Jpeg enc.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/kernels/jpeg_enc.cl - 编码 GPU kernels：
//   1) rgb_to_ycbcr_full : RGB->YCbCr（全分辨率平面，边缘复制补齐）
//   2) downsample_chroma : 4:2:0 色度 2x2 盒式下采样（libjpeg h2v2_downsample）
//   3) fdct_quantize     : 2D FDCT + 量化（自然序系数输出）
// 编译选项 -cl-std=CL1.2。

// RGB -> YCbCr（BT.601），输出全分辨率平面（含块补齐区域，边缘复制）。
// plane_w/plane_h 为输出平面尺寸（含 padding），width/height 为真实图像尺寸。
__kernel void rgb_to_ycbcr(__global const uchar* rgb, int rgb_stride,
                           __global float* y_plane, __global float* cb_plane,
                           __global float* cr_plane, int plane_stride, int width,
                           int height, int plane_w, int plane_h) {
  int x = get_global_id(0);
  int y = get_global_id(1);
  if (x >= plane_w || y >= plane_h) return;
  int rx = x < width ? x : width - 1;
  int ry = y < height ? y : height - 1;
  int idx = ry * rgb_stride + rx * 3;
  float r = rgb[idx];
  float g = rgb[idx + 1];
  float b = rgb[idx + 2];
  y_plane[y * plane_stride + x] = 0.299f * r + 0.587f * g + 0.114f * b;
  cb_plane[y * plane_stride + x] = 128.0f - 0.168736f * r - 0.331264f * g + 0.5f * b;
  cr_plane[y * plane_stride + x] = 128.0f + 0.5f * r - 0.418688f * g - 0.081312f * b;
}

// 色度盒式下采样：抽取比例由 h_ratio / v_ratio 给出，不能写死 2x2——
// 4:2:2 的色度平面与亮度等高，只做水平抽取；写死 2x2 时 sy = y*2 会越过
// 平面底部被 clamp 到同一行，下半幅色度被摊平（文件能开、亮度正常、颜色跑偏）。
// 舍入统一为 (sum + count/2) / count：h2v2 即 (a+b+c+d+2)>>2，h2v1 即 (a+b+1)>>1。
__kernel void downsample_chroma(__global const float* src, int src_stride,
                                __global float* dst, int dst_stride, int src_w,
                                int src_h, int dst_w, int dst_h, int h_ratio,
                                int v_ratio) {
  int x = get_global_id(0);
  int y = get_global_id(1);
  if (x >= dst_w || y >= dst_h) return;
  int sx = x * h_ratio;
  int sy = y * v_ratio;
  int count = h_ratio * v_ratio;
  int sum = 0;
  for (int dy = 0; dy < v_ratio; dy++) {
    for (int dx = 0; dx < h_ratio; dx++) {
      int px = sx + dx;
      int py = sy + dy;
      int cx = px < src_w ? px : src_w - 1;
      int cy = py < src_h ? py : src_h - 1;
      sum += (int)(src[cy * src_stride + cx] + 0.5f);
    }
  }
  dst[y * dst_stride + x] = (float)((sum + count / 2) / count);
}

// 2D FDCT + 量化。每个 work-item 计算一个块的一个系数。
//   plane[block 像素] 为 -128 平移后的样本；dct_mat 与解码共用同一基矩阵。
//   coeffs[coeff_off + bid*64 + p] 输出自然序量化系数（float 整数）。
__kernel void fdct_quantize(__global const float* plane, int plane_stride,
                            __global const float* qbuf, int qtable_index,
                            __global const float* dct_mat,
                            __global float* coeffs, int coeff_off,
                            int block_stride, int blocks_total) {
  uint gid = get_global_id(0);
  uint bid = gid >> 6;
  uint p = gid & 63;
  if (bid >= (uint)blocks_total) return;
  int bx = p & 7;
  int by = p >> 3;
  int block_x = (bid % (uint)block_stride) * 8;
  int block_y = (bid / (uint)block_stride) * 8;
  const __global float* m = dct_mat + (p << 6);
  float sum = 0.0f;
  for (int k = 0; k < 64; k++) {
    int lx = k & 7;
    int ly = k >> 3;
    float f = plane[(block_y + ly) * plane_stride + (block_x + lx)] - 128.0f;
    sum += m[k] * f;
  }
  float qv = qbuf[qtable_index * 64 + p];
  if (qv < 1.0f) qv = 1.0f;
  int qi = (int)floor(sum / qv + 0.5f);
  coeffs[coeff_off + bid * 64 + p] = (float)qi;
}

// 灰度 -> Y 平面（含块补齐区域，边缘复制）。
// 灰度样本本身就是 Y，不需要颜色变换；单独一个 kernel，避免让 rgb_to_ycbcr
// 去猜「输入到底几个通道」。
__kernel void gray_to_y(__global const uchar* gray, int gray_stride,
                        __global float* y_plane, int plane_stride, int width,
                        int height, int plane_w, int plane_h) {
  int x = get_global_id(0);
  int y = get_global_id(1);
  if (x >= plane_w || y >= plane_h) return;
  int rx = x < width ? x : width - 1;
  int ry = y < height ? y : height - 1;
  y_plane[y * plane_stride + x] = (float)gray[ry * gray_stride + rx];
}
