/*
 * File: jpeg_dct_ocl.cpp
 * Brief: Jpeg dct ocl.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// src/jpeg/jpeg_dct_ocl.cpp - GPU DCT/颜色变换编排与纯 CPU 参考实现。
#include "jpeg_dct_ocl.h"

#include "ocl_device.h"
#include "ocl_program.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace image_codec {
namespace jpeg {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 64x64 DCT 基矩阵（含 1/4 归一化与 C(u)C(v)）：m[p][j]，
// p = v*8+u（自然序系数），j = y*8+x（自然序像素）。
void build_dct_matrix(float m[64][64]) {
  for (int p = 0; p < 64; p++) {
    const int u = p & 7;
    const int v = p >> 3;
    const float cu = (u == 0) ? 0.70710678f : 1.0f;
    const float cv = (v == 0) ? 0.70710678f : 1.0f;
    for (int j = 0; j < 64; j++) {
      const int x = j & 7;
      const int y = j >> 3;
      m[p][j] = 0.25f * cu * cv * std::cos(kPi * (2 * x + 1) * u / 16.0f) *
                std::cos(kPi * (2 * y + 1) * v / 16.0f);
    }
  }
}

// ---- GPU 状态（惰性初始化单例）----

struct GpuState {
  OclDevice dev;
  cl_program dec_prog = nullptr;
  cl_program enc_prog = nullptr;
  cl_kernel k_idct = nullptr;
  cl_kernel k_ycbcr = nullptr;
  cl_kernel k_rgb2y = nullptr;
  cl_kernel k_gray2y = nullptr;
  cl_kernel k_ds = nullptr;
  cl_kernel k_fdct = nullptr;
  cl_mem dct_mat = nullptr; // 解码/编码共用基矩阵
  bool ok = false;
  std::string err;
};

bool init_gpu(GpuState &g) {
  cl_int err = g.dev.init(0);
  if (err != CL_SUCCESS) {
    g.err =
        std::string("OpenCL device init failed: ") + OclProgram::errorName(err);
    return false;
  }
  std::string log;
  err = OclProgram::build(g.dev.context(), g.dev.device(), kJpegDecSource,
                          "-cl-std=CL1.2", &g.dec_prog, &log);
  if (err != CL_SUCCESS) {
    g.err = "decode kernel build failed: " + log;
    return false;
  }
  err = OclProgram::build(g.dev.context(), g.dev.device(), kJpegEncSource,
                          "-cl-std=CL1.2", &g.enc_prog, &log);
  if (err != CL_SUCCESS) {
    g.err = "encode kernel build failed: " + log;
    return false;
  }
  err = OclProgram::createKernel(g.dec_prog, "dequant_idct", &g.k_idct);
  if (err != CL_SUCCESS) {
    g.err = "createKernel(dequant_idct) failed";
    return false;
  }
  err = OclProgram::createKernel(g.dec_prog, "ycbcr_to_rgb", &g.k_ycbcr);
  if (err != CL_SUCCESS) {
    g.err = "createKernel(ycbcr_to_rgb) failed";
    return false;
  }
  err = OclProgram::createKernel(g.enc_prog, "rgb_to_ycbcr", &g.k_rgb2y);
  if (err != CL_SUCCESS) {
    g.err = "createKernel(rgb_to_ycbcr) failed";
    return false;
  }
  err = OclProgram::createKernel(g.enc_prog, "gray_to_y", &g.k_gray2y);
  if (err != CL_SUCCESS) {
    g.err = "createKernel(gray_to_y) failed";
    return false;
  }
  err = OclProgram::createKernel(g.enc_prog, "downsample_chroma", &g.k_ds);
  if (err != CL_SUCCESS) {
    g.err = "createKernel(downsample_chroma) failed";
    return false;
  }
  err = OclProgram::createKernel(g.enc_prog, "fdct_quantize", &g.k_fdct);
  if (err != CL_SUCCESS) {
    g.err = "createKernel(fdct_quantize) failed";
    return false;
  }
  float m[64][64];
  build_dct_matrix(m);
  g.dct_mat =
      clCreateBuffer(g.dev.context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                     sizeof(m), m, &err);
  if (err != CL_SUCCESS || g.dct_mat == nullptr) {
    g.err = "clCreateBuffer(dct_mat) failed";
    return false;
  }
  g.ok = true;
  return true;
}

GpuState &gpu() {
  static GpuState g;
  static bool tried = false;
  if (!tried) {
    tried = true;
    init_gpu(g);
  }
  return g;
}

void check_cl(cl_int err, const char *what, std::string *err_out) {
  if (err != CL_SUCCESS && err_out) {
    *err_out = std::string(what) + ": " + OclProgram::errorName(err);
  }
}

} // namespace

// ---- GPU 解码 ----

int jpeg_gpu_decode(const JpegImage &img, const std::vector<float> &coeffs,
                    std::vector<uint8_t> *rgb, std::string *err) {
  GpuState &g = gpu();
  if (!g.ok) {
    if (err) *err = g.err;
    return -1;
  }
  cl_int e = CL_SUCCESS;

  // 量化表（4 张，自然序）。
  float qtable[4 * 64] = {0.f};
  for (int t = 0; t < 4; t++) {
    if (img.quant[t].present) {
      for (int i = 0; i < 64; i++) {
        qtable[t * 64 + i] = static_cast<float>(img.quant[t].q[i]);
      }
    }
  }

  int plane_floats = 0;
  for (int c = 0; c < img.ncomp; c++) {
    const JpegComponentInfo &ci = img.comp[c];
    plane_floats += (ci.blocks_x * 8) * (ci.blocks_y * 8);
  }

  cl_mem coeff_buf =
      clCreateBuffer(g.dev.context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                     static_cast<size_t>(coeffs.size()) * sizeof(float),
                     const_cast<float *>(coeffs.data()), &e);
  check_cl(e, "clCreateBuffer(coeffs)", err);
  if (e != CL_SUCCESS) return -1;
  cl_mem qbuf =
      clCreateBuffer(g.dev.context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                     sizeof(qtable), qtable, &e);
  check_cl(e, "clCreateBuffer(qtable)", err);
  if (e != CL_SUCCESS) {
    clReleaseMemObject(coeff_buf);
    return -1;
  }
  cl_mem plane_buf = clCreateBuffer(
      g.dev.context(), CL_MEM_READ_WRITE,
      static_cast<size_t>(plane_floats) * sizeof(float), nullptr, &e);
  check_cl(e, "clCreateBuffer(planes)", err);
  if (e != CL_SUCCESS) {
    clReleaseMemObject(coeff_buf);
    clReleaseMemObject(qbuf);
    return -1;
  }

  // 反量化 + IDCT：每分量一次。
  for (int c = 0; c < img.ncomp; c++) {
    const JpegComponentInfo &ci = img.comp[c];
    const int blocks = ci.blocks_x * ci.blocks_y;
    const int argi = 0;
    clSetKernelArg(g.k_idct, argi, sizeof(cl_mem), &coeff_buf);
    clSetKernelArg(g.k_idct, 1, sizeof(int), &ci.coeff_off);
    clSetKernelArg(g.k_idct, 2, sizeof(cl_mem), &qbuf);
    clSetKernelArg(g.k_idct, 3, sizeof(cl_mem), &g.dct_mat);
    clSetKernelArg(g.k_idct, 4, sizeof(cl_mem), &plane_buf);
    clSetKernelArg(g.k_idct, 5, sizeof(int), &ci.plane_off);
    int plane_stride = ci.blocks_x * 8;
    clSetKernelArg(g.k_idct, 6, sizeof(int), &plane_stride);
    clSetKernelArg(g.k_idct, 7, sizeof(int), &ci.blocks_x);
    clSetKernelArg(g.k_idct, 8, sizeof(int), &blocks);
    int tq = ci.tq;
    clSetKernelArg(g.k_idct, 9, sizeof(int), &tq);
    size_t gsize = static_cast<size_t>(blocks) * 64;
    e = clEnqueueNDRangeKernel(g.dev.queue(), g.k_idct, 1, nullptr, &gsize,
                               nullptr, 0, nullptr, nullptr);
    if (e != CL_SUCCESS) {
      if (err)
        *err = std::string("dequant_idct enqueue failed: ") +
               OclProgram::errorName(e);
      clReleaseMemObject(coeff_buf);
      clReleaseMemObject(qbuf);
      clReleaseMemObject(plane_buf);
      return -1;
    }
    (void)argi;
  }

  // YCbCr -> RGB + 上采样。
  int desc[3 * 6] = {0};
  for (int c = 0; c < 3; c++) {
    if (c < img.ncomp) {
      const JpegComponentInfo &ci = img.comp[c];
      desc[c * 6 + 0] = ci.plane_off;
      desc[c * 6 + 1] = ci.blocks_x * 8;
      desc[c * 6 + 2] = ci.plane_w;
      desc[c * 6 + 3] = ci.plane_h;
      desc[c * 6 + 4] = ci.h_samp;
      desc[c * 6 + 5] = ci.v_samp;
    } else {
      desc[c * 6 + 0] = 0;
      desc[c * 6 + 1] = 0;
      desc[c * 6 + 2] = 1;
      desc[c * 6 + 3] = 1;
      desc[c * 6 + 4] = img.max_h;
      desc[c * 6 + 5] = img.max_v;
    }
  }
  cl_mem desc_buf =
      clCreateBuffer(g.dev.context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                     sizeof(desc), desc, &e);
  check_cl(e, "clCreateBuffer(desc)", err);
  if (e != CL_SUCCESS) {
    clReleaseMemObject(coeff_buf);
    clReleaseMemObject(qbuf);
    clReleaseMemObject(plane_buf);
    return -1;
  }

  // 通道保真：单分量（灰度）JPEG 输出 1 字节/像素。
  const int out_channels = (img.ncomp == 1) ? 1 : 3;
  const size_t out_bytes = static_cast<size_t>(img.width) * img.height *
                           static_cast<size_t>(out_channels);
  rgb->assign(out_bytes, 0);
  cl_mem out_buf = clCreateBuffer(g.dev.context(), CL_MEM_WRITE_ONLY, out_bytes,
                                  nullptr, &e);
  check_cl(e, "clCreateBuffer(rgb)", err);
  if (e != CL_SUCCESS) {
    clReleaseMemObject(coeff_buf);
    clReleaseMemObject(qbuf);
    clReleaseMemObject(plane_buf);
    clReleaseMemObject(desc_buf);
    return -1;
  }

  clSetKernelArg(g.k_ycbcr, 0, sizeof(cl_mem), &plane_buf);
  clSetKernelArg(g.k_ycbcr, 1, sizeof(cl_mem), &desc_buf);
  int mh = img.max_h, mv = img.max_v, nc = img.ncomp;
  clSetKernelArg(g.k_ycbcr, 2, sizeof(int), &mh);
  clSetKernelArg(g.k_ycbcr, 3, sizeof(int), &mv);
  clSetKernelArg(g.k_ycbcr, 4, sizeof(int), &nc);
  clSetKernelArg(g.k_ycbcr, 5, sizeof(cl_mem), &out_buf);
  int ostride = img.width * out_channels;
  clSetKernelArg(g.k_ycbcr, 6, sizeof(int), &ostride);
  clSetKernelArg(g.k_ycbcr, 7, sizeof(int), &img.width);
  clSetKernelArg(g.k_ycbcr, 8, sizeof(int), &img.height);
  clSetKernelArg(g.k_ycbcr, 9, sizeof(int), &out_channels);
  size_t gdim[2] = {static_cast<size_t>(img.width),
                    static_cast<size_t>(img.height)};
  e = clEnqueueNDRangeKernel(g.dev.queue(), g.k_ycbcr, 2, nullptr, gdim,
                             nullptr, 0, nullptr, nullptr);
  if (e != CL_SUCCESS) {
    if (err)
      *err = std::string("ycbcr_to_rgb enqueue failed: ") +
             OclProgram::errorName(e);
    clReleaseMemObject(coeff_buf);
    clReleaseMemObject(qbuf);
    clReleaseMemObject(plane_buf);
    clReleaseMemObject(desc_buf);
    clReleaseMemObject(out_buf);
    return -1;
  }
  e = clEnqueueReadBuffer(g.dev.queue(), out_buf, CL_TRUE, 0, out_bytes,
                          rgb->data(), 0, nullptr, nullptr);
  if (e != CL_SUCCESS) {
    if (err) *err = std::string("read rgb failed: ") + OclProgram::errorName(e);
    clReleaseMemObject(coeff_buf);
    clReleaseMemObject(qbuf);
    clReleaseMemObject(plane_buf);
    clReleaseMemObject(desc_buf);
    clReleaseMemObject(out_buf);
    return -1;
  }

  clReleaseMemObject(coeff_buf);
  clReleaseMemObject(qbuf);
  clReleaseMemObject(plane_buf);
  clReleaseMemObject(desc_buf);
  clReleaseMemObject(out_buf);
  return 0;
}

// ---- GPU 编码 ----

int jpeg_gpu_encode(const JpegImage &img, const uint8_t *rgb,
                    std::vector<float> *coeffs, std::string *err) {
  GpuState &g = gpu();
  if (!g.ok) {
    if (err) *err = g.err;
    return -1;
  }
  cl_int e = CL_SUCCESS;

  float qtable[4 * 64] = {0.f};
  for (int t = 0; t < 4; t++) {
    if (img.quant[t].present) {
      for (int i = 0; i < 64; i++) {
        qtable[t * 64 + i] = static_cast<float>(img.quant[t].q[i]);
      }
    }
  }

  // 全分辨率平面（所有分量共用 padded 尺寸）。
  const int full_w = img.mcus_x * img.max_h * 8;
  const int full_h = img.mcus_y * img.max_v * 8;
  const int full_floats = full_w * full_h;

  const int in_channels = (img.ncomp == 1) ? 1 : 3;
  const size_t rgb_bytes = static_cast<size_t>(img.width) * img.height *
                           static_cast<size_t>(in_channels);
  cl_mem rgb_buf =
      clCreateBuffer(g.dev.context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                     rgb_bytes, const_cast<uint8_t *>(rgb), &e);
  check_cl(e, "clCreateBuffer(rgb)", err);
  if (e != CL_SUCCESS) return -1;

  cl_mem y_buf = clCreateBuffer(
      g.dev.context(), CL_MEM_READ_WRITE,
      static_cast<size_t>(full_floats) * sizeof(float), nullptr, &e);
  check_cl(e, "clCreateBuffer(y)", err);
  if (e != CL_SUCCESS) {
    clReleaseMemObject(rgb_buf);
    return -1;
  }
  cl_mem cb_buf = clCreateBuffer(
      g.dev.context(), CL_MEM_READ_WRITE,
      static_cast<size_t>(full_floats) * sizeof(float), nullptr, &e);
  cl_mem cr_buf = clCreateBuffer(
      g.dev.context(), CL_MEM_READ_WRITE,
      static_cast<size_t>(full_floats) * sizeof(float), nullptr, &e);
  check_cl(e, "clCreateBuffer(cb/cr)", err);
  if (e != CL_SUCCESS) {
    clReleaseMemObject(rgb_buf);
    clReleaseMemObject(y_buf);
    clReleaseMemObject(cb_buf);
    clReleaseMemObject(cr_buf);
    return -1;
  }

  size_t gdim2[2] = {static_cast<size_t>(full_w), static_cast<size_t>(full_h)};
  if (img.ncomp == 1) {
    // 灰度：样本直接就是 Y，不需要颜色变换与色度平面。
    clSetKernelArg(g.k_gray2y, 0, sizeof(cl_mem), &rgb_buf);
    int gray_stride = img.width;
    clSetKernelArg(g.k_gray2y, 1, sizeof(int), &gray_stride);
    clSetKernelArg(g.k_gray2y, 2, sizeof(cl_mem), &y_buf);
    clSetKernelArg(g.k_gray2y, 3, sizeof(int), &full_w);
    clSetKernelArg(g.k_gray2y, 4, sizeof(int), &img.width);
    clSetKernelArg(g.k_gray2y, 5, sizeof(int), &img.height);
    clSetKernelArg(g.k_gray2y, 6, sizeof(int), &full_w);
    clSetKernelArg(g.k_gray2y, 7, sizeof(int), &full_h);
    e = clEnqueueNDRangeKernel(g.dev.queue(), g.k_gray2y, 2, nullptr, gdim2,
                               nullptr, 0, nullptr, nullptr);
    if (e != CL_SUCCESS) {
      if (err)
        *err = std::string("gray_to_y enqueue failed: ") +
               OclProgram::errorName(e);
      clReleaseMemObject(rgb_buf);
      clReleaseMemObject(y_buf);
      clReleaseMemObject(cb_buf);
      clReleaseMemObject(cr_buf);
      return -1;
    }
  } else {
    // RGB -> YCbCr（全分辨率，边缘复制补齐）。
    clSetKernelArg(g.k_rgb2y, 0, sizeof(cl_mem), &rgb_buf);
    int rgb_stride = img.width * 3;
    clSetKernelArg(g.k_rgb2y, 1, sizeof(int), &rgb_stride);
    clSetKernelArg(g.k_rgb2y, 2, sizeof(cl_mem), &y_buf);
    clSetKernelArg(g.k_rgb2y, 3, sizeof(cl_mem), &cb_buf);
    clSetKernelArg(g.k_rgb2y, 4, sizeof(cl_mem), &cr_buf);
    clSetKernelArg(g.k_rgb2y, 5, sizeof(int), &full_w);
    clSetKernelArg(g.k_rgb2y, 6, sizeof(int), &img.width);
    clSetKernelArg(g.k_rgb2y, 7, sizeof(int), &img.height);
    clSetKernelArg(g.k_rgb2y, 8, sizeof(int), &full_w);
    clSetKernelArg(g.k_rgb2y, 9, sizeof(int), &full_h);
    e = clEnqueueNDRangeKernel(g.dev.queue(), g.k_rgb2y, 2, nullptr, gdim2,
                               nullptr, 0, nullptr, nullptr);
    if (e != CL_SUCCESS) {
      if (err)
        *err = std::string("rgb_to_ycbcr enqueue failed: ") +
               OclProgram::errorName(e);
      clReleaseMemObject(rgb_buf);
      clReleaseMemObject(y_buf);
      clReleaseMemObject(cb_buf);
      clReleaseMemObject(cr_buf);
      return -1;
    }
  }

  // 色度下采样（非 4:4:4 时）。
  const int ncomp = img.ncomp;
  bool need_ds = false;
  for (int c = 0; c < ncomp; c++) {
    if (img.comp[c].h_samp != img.max_h || img.comp[c].v_samp != img.max_v) {
      need_ds = true;
    }
  }
  cl_mem ds_cb = nullptr;
  cl_mem ds_cr = nullptr;
  if (need_ds) {
    const JpegComponentInfo &c1 = img.comp[1];
    const int ds_floats = (c1.blocks_x * 8) * (c1.blocks_y * 8);
    ds_cb = clCreateBuffer(g.dev.context(), CL_MEM_READ_WRITE,
                           static_cast<size_t>(ds_floats) * sizeof(float),
                           nullptr, &e);
    ds_cr = clCreateBuffer(g.dev.context(), CL_MEM_READ_WRITE,
                           static_cast<size_t>(ds_floats) * sizeof(float),
                           nullptr, &e);
    check_cl(e, "clCreateBuffer(ds)", err);
    if (e != CL_SUCCESS) {
      clReleaseMemObject(rgb_buf);
      clReleaseMemObject(y_buf);
      clReleaseMemObject(cb_buf);
      clReleaseMemObject(cr_buf);
      return -1;
    }
    // 抽取比例与着色器一致，从采样因子推导：4:2:2 只做水平抽取（v_ratio=1），
    // 4:2:0 才是 2x2。写死 2x2 会让 4:2:2 的下半幅色度被 clamp 到同一行。
    const int h_ratio = img.max_h / (c1.h_samp > 0 ? c1.h_samp : 1);
    const int v_ratio = img.max_v / (c1.v_samp > 0 ? c1.v_samp : 1);
    for (int ch = 0; ch < 2; ch++) {
      cl_mem src = (ch == 0) ? cb_buf : cr_buf;
      cl_mem dst = (ch == 0) ? ds_cb : ds_cr;
      clSetKernelArg(g.k_ds, 0, sizeof(cl_mem), &src);
      clSetKernelArg(g.k_ds, 1, sizeof(int), &full_w);
      clSetKernelArg(g.k_ds, 2, sizeof(cl_mem), &dst);
      int ds_stride = c1.blocks_x * 8;
      clSetKernelArg(g.k_ds, 3, sizeof(int), &ds_stride);
      clSetKernelArg(g.k_ds, 4, sizeof(int), &full_w);
      clSetKernelArg(g.k_ds, 5, sizeof(int), &full_h);
      int ds_w = c1.blocks_x * 8;
      int ds_h = c1.blocks_y * 8;
      clSetKernelArg(g.k_ds, 6, sizeof(int), &ds_w);
      clSetKernelArg(g.k_ds, 7, sizeof(int), &ds_h);
      int h_ratio_arg = h_ratio < 1 ? 1 : h_ratio;
      int v_ratio_arg = v_ratio < 1 ? 1 : v_ratio;
      clSetKernelArg(g.k_ds, 8, sizeof(int), &h_ratio_arg);
      clSetKernelArg(g.k_ds, 9, sizeof(int), &v_ratio_arg);
      size_t dg[2] = {static_cast<size_t>(ds_w), static_cast<size_t>(ds_h)};
      e = clEnqueueNDRangeKernel(g.dev.queue(), g.k_ds, 2, nullptr, dg, nullptr,
                                 0, nullptr, nullptr);
      if (e != CL_SUCCESS) {
        if (err)
          *err = std::string("downsample enqueue failed: ") +
                 OclProgram::errorName(e);
        clReleaseMemObject(rgb_buf);
        clReleaseMemObject(y_buf);
        clReleaseMemObject(cb_buf);
        clReleaseMemObject(cr_buf);
        clReleaseMemObject(ds_cb);
        clReleaseMemObject(ds_cr);
        return -1;
      }
    }
  }

  cl_mem qbuf =
      clCreateBuffer(g.dev.context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                     sizeof(qtable), qtable, &e);
  check_cl(e, "clCreateBuffer(qtable)", err);
  if (e != CL_SUCCESS) {
    clReleaseMemObject(rgb_buf);
    clReleaseMemObject(y_buf);
    clReleaseMemObject(cb_buf);
    clReleaseMemObject(cr_buf);
    if (ds_cb) clReleaseMemObject(ds_cb);
    if (ds_cr) clReleaseMemObject(ds_cr);
    return -1;
  }
  cl_mem coeff_buf = clCreateBuffer(
      g.dev.context(), CL_MEM_READ_WRITE,
      static_cast<size_t>(img.total_blocks) * 64 * sizeof(float), nullptr, &e);
  check_cl(e, "clCreateBuffer(coeffs)", err);
  if (e != CL_SUCCESS) {
    clReleaseMemObject(rgb_buf);
    clReleaseMemObject(y_buf);
    clReleaseMemObject(cb_buf);
    clReleaseMemObject(cr_buf);
    if (ds_cb) clReleaseMemObject(ds_cb);
    if (ds_cr) clReleaseMemObject(ds_cr);
    clReleaseMemObject(qbuf);
    return -1;
  }

  // FDCT + 量化：每分量一次。
  for (int c = 0; c < ncomp; c++) {
    const JpegComponentInfo &ci = img.comp[c];
    cl_mem plane;
    if (c == 0) {
      plane = y_buf;
    } else if (c == 1) {
      plane = (need_ds) ? ds_cb : cb_buf;
    } else {
      plane = (need_ds) ? ds_cr : cr_buf;
    }
    int plane_stride = (c == 0 || !need_ds) ? full_w : (ci.blocks_x * 8);
    const int blocks = ci.blocks_x * ci.blocks_y;
    clSetKernelArg(g.k_fdct, 0, sizeof(cl_mem), &plane);
    clSetKernelArg(g.k_fdct, 1, sizeof(int), &plane_stride);
    clSetKernelArg(g.k_fdct, 2, sizeof(cl_mem), &qbuf);
    int tq = ci.tq;
    clSetKernelArg(g.k_fdct, 3, sizeof(int), &tq);
    clSetKernelArg(g.k_fdct, 4, sizeof(cl_mem), &g.dct_mat);
    clSetKernelArg(g.k_fdct, 5, sizeof(cl_mem), &coeff_buf);
    clSetKernelArg(g.k_fdct, 6, sizeof(int), &ci.coeff_off);
    clSetKernelArg(g.k_fdct, 7, sizeof(int), &ci.blocks_x);
    clSetKernelArg(g.k_fdct, 8, sizeof(int), &blocks);
    size_t gsz = static_cast<size_t>(blocks) * 64;
    e = clEnqueueNDRangeKernel(g.dev.queue(), g.k_fdct, 1, nullptr, &gsz,
                               nullptr, 0, nullptr, nullptr);
    if (e != CL_SUCCESS) {
      if (err)
        *err = std::string("fdct enqueue failed: ") + OclProgram::errorName(e);
      clReleaseMemObject(rgb_buf);
      clReleaseMemObject(y_buf);
      clReleaseMemObject(cb_buf);
      clReleaseMemObject(cr_buf);
      if (ds_cb) clReleaseMemObject(ds_cb);
      if (ds_cr) clReleaseMemObject(ds_cr);
      clReleaseMemObject(qbuf);
      clReleaseMemObject(coeff_buf);
      return -1;
    }
  }

  coeffs->assign(static_cast<size_t>(img.total_blocks) * 64, 0.f);
  e = clEnqueueReadBuffer(g.dev.queue(), coeff_buf, CL_TRUE, 0,
                          static_cast<size_t>(img.total_blocks) * 64 *
                              sizeof(float),
                          coeffs->data(), 0, nullptr, nullptr);
  if (e != CL_SUCCESS) {
    if (err)
      *err = std::string("read coeffs failed: ") + OclProgram::errorName(e);
  }

  clReleaseMemObject(rgb_buf);
  clReleaseMemObject(y_buf);
  clReleaseMemObject(cb_buf);
  clReleaseMemObject(cr_buf);
  if (ds_cb) clReleaseMemObject(ds_cb);
  if (ds_cr) clReleaseMemObject(ds_cr);
  clReleaseMemObject(qbuf);
  clReleaseMemObject(coeff_buf);
  return (e == CL_SUCCESS) ? 0 : -1;
}

// ---- 纯 CPU 参考解码 ----

namespace {

int clampi(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return v;
}

// 与 GPU kernel 相同的逐像素上采样/颜色变换逻辑（CPU 参考）。
void cpu_ycbcr_to_rgb(const JpegImage &img, const float *planes,
                      std::vector<uint8_t> *rgb) {
  auto rd = [&](int c, int cx, int cy) -> int {
    const JpegComponentInfo &ci = img.comp[c];
    const int stride = ci.blocks_x * 8;
    if (cx < 0) cx = 0;
    if (cx >= ci.plane_w) cx = ci.plane_w - 1;
    if (cy < 0) cy = 0;
    if (cy >= ci.plane_h) cy = ci.plane_h - 1;
    const float v = planes[ci.plane_off + cy * stride + cx];
    return static_cast<int>(v); // 已为整数样本
  };

  auto sample = [&](int c, int x, int y) -> int {
    const JpegComponentInfo &ci = img.comp[c];
    const int stride = ci.blocks_x * 8;
    if (ci.h_samp == img.max_h && ci.v_samp == img.max_v) {
      return rd(c, x, y);
    }
    const int hratio = img.max_h / ci.h_samp;
    const int vratio = img.max_v / ci.v_samp;
    const auto *p = planes + ci.plane_off;
    if (hratio == 2 && vratio == 2) {
      int cx = x >> 1, cy = y >> 1, h = x & 1, v = y & 1;
      if (cx > ci.plane_w - 1) cx = ci.plane_w - 1;
      if (cy > ci.plane_h - 1) cy = ci.plane_h - 1;
      const int iy = cy;
      const int oy2 = (v == 0) ? (cy > 0 ? cy - 1 : cy)
                               : (cy + 1 < ci.plane_h ? cy + 1 : cy);
      auto s = [&](int k, int row) { return 3 * rd(c, k, row); };
      const int s_cur = 3 * rd(c, cx, iy) + rd(c, cx, oy2);
      if (h == 0) {
        if (cx == 0) return (4 * s_cur + 8) >> 4;
        return (3 * s_cur + s(cx - 1, iy) + rd(c, cx - 1, oy2) + 8) >> 4;
      }
      if (cx >= ci.plane_w - 1) return (4 * s_cur + 7) >> 4;
      return (3 * s_cur + s(cx + 1, iy) + rd(c, cx + 1, oy2) + 7) >> 4;
    }
    if (hratio == 2 && vratio == 1) {
      int cx = x >> 1, h = x & 1;
      if (cx > ci.plane_w - 1) cx = ci.plane_w - 1;
      const int cur = rd(c, cx, y);
      if (h == 0) {
        if (cx == 0) return cur;
        return (3 * cur + rd(c, cx - 1, y) + 1) >> 2;
      }
      if (cx >= ci.plane_w - 1) return cur;
      return (3 * cur + rd(c, cx + 1, y) + 2) >> 2;
    }
    return rd(c, x * ci.h_samp / img.max_h, y * ci.v_samp / img.max_v);
  };

  if (img.ncomp < 3) {
    // 单分量：样本就是灰度值，直接输出 1 通道（与 GPU kernel 的 out_channels==1 一致）。
    rgb->assign(static_cast<size_t>(img.width) * img.height, 0);
    for (int y = 0; y < img.height; y++) {
      for (int x = 0; x < img.width; x++) {
        (*rgb)[static_cast<size_t>(y) * img.width + x] =
            static_cast<uint8_t>(clampi(sample(0, x, y)));
      }
    }
    return;
  }
  rgb->assign(static_cast<size_t>(img.width) * img.height * 3, 0);
  for (int y = 0; y < img.height; y++) {
    for (int x = 0; x < img.width; x++) {
      const int yv = sample(0, x, y);
      int r, g, b;
      if (img.ncomp >= 3) {
        const int cb = sample(1, x, y) - 128;
        const int cr = sample(2, x, y) - 128;
        r = clampi(yv + ((91881 * cr + 32768) >> 16));
        g = clampi(yv - ((46802 * cr + 32768) >> 16) -
                   ((22554 * cb + 32768) >> 16));
        b = clampi(yv + ((116130 * cb + 32768) >> 16));
      } else {
        r = g = b = clampi(yv);
      }
      uint8_t *o = &(*rgb)[(y * img.width + x) * 3];
      o[0] = static_cast<uint8_t>(r);
      o[1] = static_cast<uint8_t>(g);
      o[2] = static_cast<uint8_t>(b);
    }
  }
}

} // namespace

int jpeg_cpu_decode(const JpegImage &img, const std::vector<float> &coeffs,
                    std::vector<uint8_t> *rgb, std::string *err) {
  float m[64][64];
  build_dct_matrix(m);
  std::vector<float> planes(
      static_cast<size_t>(img.total_blocks) * 64 * 4 + 1024, 0.f);
  // 每分量 IDCT。
  for (int c = 0; c < img.ncomp; c++) {
    const JpegComponentInfo &ci = img.comp[c];
    const int stride = ci.blocks_x * 8;
    const int blocks = ci.blocks_x * ci.blocks_y;
    for (int bid = 0; bid < blocks; bid++) {
      const float *blk = &coeffs[ci.coeff_off + bid * 64];
      const int bx = bid % ci.blocks_x;
      const int by = bid / ci.blocks_x;
      for (int p = 0; p < 64; p++) {
        float sum = 0.f;
        for (int k = 0; k < 64; k++) {
          // IDCT 基矩阵按 [系数][像素] 存放，此处沿列求和（m[k][p]）。
          sum += m[k][p] * blk[k] * static_cast<float>(img.quant[ci.tq].q[k]);
        }
        // 解码端 IDCT 输出为 [-128,127] 电平，加 128 还原样本值。
        int iv = static_cast<int>(std::floor(sum + 128.0f + 0.5f));
        if (iv < 0) iv = 0;
        if (iv > 255) iv = 255;
        const int px = (bx * 8 + (p & 7));
        const int py = (by * 8 + (p >> 3));
        planes[ci.plane_off + py * stride + px] = static_cast<float>(iv);
      }
    }
  }
  cpu_ycbcr_to_rgb(img, planes.data(), rgb);
  return 0;
}

} // namespace jpeg
} // namespace image_codec
