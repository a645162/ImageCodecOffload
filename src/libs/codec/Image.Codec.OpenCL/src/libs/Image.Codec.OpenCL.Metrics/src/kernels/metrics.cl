/*
 * File: metrics.cl
 * Brief: Metrics.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// metrics.cl - MSE / SSIM / MS-SSIM 的 OpenCL kernel（单尺度累加）。
//
// 并行形态：一个工作项负责一个 (像素, 通道)。
//   * with_window == 0（只要 MSE/PSNR）：只做减法-平方，每像素 2 次读；
//   * with_window != 0（SSIM / MS-SSIM）：在自己的 11x11 高斯窗上直接求和。
//
// 为什么不做「先可分离模糊成 5 张平面再合成」：那需要 5 张与图等大的 float 平面
// （32000x19656 的图就是 12.5 GB），而我们这里连整幅图都不需要上传——按行 band
// 上传即可。直方窗口的代价是每像素 2x121 次读，全部命中 L2，远低于一次大图往返。
//
// 边界：复制边缘（replicate），与 metrics_cpu.cpp 的参考实现完全一致。
//
// 权重与累加量统一用 double：float 会让均值化的 SSIM 差到 1e-6 量级，
// 只能靠放宽容差来「通过」对拍，那不是对齐。因此本 kernel 需要 fp64 支持，
// 宿主侧在启动前检查 CL_DEVICE_DOUBLE_FP_CONFIG。
#pragma OPENCL EXTENSION cl_khr_fp64 : enable

#define WIN 11
#define WSZ 11

__kernel void metrics_band(__global const uchar *a,
                           __global const uchar *b,
                           __global const double *win,
                           const int width,
                           const int height,
                           const int channels,
                           const int y0,
                           const int band_rows,
                           const int halo,
                           const int with_window,
                           __global double *partials) {
  const int lid = (int)get_local_id(0);
  const int lsize = (int)get_local_size(0);
  const int top = y0 - halo;

  const long tiles = (long)width * channels;
  const long total = (long)band_rows * tiles;

  double local_ssim = 0.0;
  double local_cs = 0.0;
  double local_sq = 0.0;

  for (long index = (long)get_global_id(0); index < total;
       index += (long)get_global_size(0)) {
    // 整图行号 gy 与 band 缓冲内的本地行号 ly = gy - top 必须分开：
    // 缓冲第 0 行对应整图 y0-halo 行（host 按同一规则填充并复制边缘）。
    const int gy = y0 + (int)(index / tiles);
    const long rem = index % tiles;
    const int x = (int)(rem / channels);
    const int c = (int)(rem % channels);

    const int ly = gy - top;
    const long offset = ((long)ly * width + x) * channels + c;

    const double d =
        (double)a[offset] - (double)b[offset];
    local_sq += d * d;

    if (with_window == 0) continue;

    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double syy = 0.0;
    double sxy = 0.0;

    for (int dy = -halo; dy <= halo; ++dy) {
      int cy = gy + dy;
      cy = cy < 0 ? 0 : (cy >= height ? height - 1 : cy);
      const int row = cy - top;
      const double wy = win[dy + halo];
      const long row_base = (long)row * width * channels;

      for (int dx = -halo; dx <= halo; ++dx) {
        int cx = x + dx;
        cx = cx < 0 ? 0 : (cx >= width ? width - 1 : cx);

        const long o = row_base + (long)cx * channels + c;
        const double px = (double)a[o];
        const double py = (double)b[o];
        const double g = wy * win[dx + halo];

        sx += g * px;
        sy += g * py;
        sxx += g * px * px;
        syy += g * py * py;
        sxy += g * px * py;
      }
    }

    const double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
    const double kC2 = (0.03 * 255.0) * (0.03 * 255.0);

    const double vx = sxx - sx * sx;
    const double vy = syy - sy * sy;
    const double vxy = sxy - sx * sy;

    const double lum = (2.0 * sx * sy + kC1) / (sx * sx + sy * sy + kC1);
    const double den = vx + vy + kC2;
    const double cs = den > 0.0 ? (2.0 * vxy + kC2) / den : 0.0;

    local_cs += cs;
    local_ssim += lum * cs;
  }

  // work-group 内归约（三份 double）。宿主固定 local size = 256，
  // 与这里的数组尺寸一致；改一处必须改另一处。
  __local double s_ssim[256];
  __local double s_cs[256];
  __local double s_sq[256];

  s_ssim[lid] = local_ssim;
  s_cs[lid] = local_cs;
  s_sq[lid] = local_sq;
  barrier(CLK_LOCAL_MEM_FENCE);

  for (int stride = lsize / 2; stride > 0; stride >>= 1) {
    if (lid < stride) {
      s_ssim[lid] += s_ssim[lid + stride];
      s_cs[lid] += s_cs[lid + stride];
      s_sq[lid] += s_sq[lid + stride];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if (lid == 0) {
    const int ngroups = (int)get_num_groups(0);
    const int g = (int)get_group_id(0);
    partials[g] = s_ssim[0];
    partials[ngroups + g] = s_cs[0];
    partials[2 * ngroups + g] = s_sq[0];
  }
}
