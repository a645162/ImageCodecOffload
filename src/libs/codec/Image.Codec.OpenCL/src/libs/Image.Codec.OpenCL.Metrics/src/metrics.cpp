/*
 * File: metrics.cpp
 * Brief: Metrics.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// metrics.cpp - MSE / PSNR / SSIM / MS-SSIM 的 OpenCL 实现（按行 band 流式上传）。
//
// 为什么按 band 而不是整幅上传：Merge.bmp 是 32000x19656x3（1.89 GB），
// 两张图同时驻留设备就是 3.8 GB；按 band 上传后显存占用只与 band_rows 有关、
// 与图像高度无关，大图与小图走同一条代码路径。
#include <CL/cl.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

#include "metrics.h"
#include "metrics_common.h"
#include "ocl_device.h"
#include "ocl_program.h"

namespace image_codec {
namespace ocl {
namespace metrics {

// 由 metrics_cl.cpp.in 生成（CMake configure_file）。
extern const char *metrics_cl_source;

namespace {

// kernel 内 work-group 归约按 256 写死（见 kernels/metrics.cl 的 __local 数组）。
constexpr size_t kLocalSize = 256;
// 上限只是防止 global size 无意义地铺满超大图；每工作项本来就是网格跨步循环。
constexpr size_t kMaxGroups = 4096;

bool validate(const ImageView &a, const ImageView &b, std::string *err) {
  if (!a.valid() || !b.valid()) {
    if (err != nullptr) *err = "metrics: invalid image buffer";
    return false;
  }
  if (a.width != b.width || a.height != b.height || a.channels != b.channels) {
    if (err != nullptr) *err = "metrics: size/channel mismatch";
    return false;
  }
  // kernel 只有一份行跨度参数；两份数据的行字节不同就无法共用同一条取址。
  if (a.rowBytes() != b.rowBytes()) {
    if (err != nullptr) *err = "metrics: row stride mismatch";
    return false;
  }
  if (static_cast<long long>(a.width) * a.channels > INT_MAX / 2) {
    if (err != nullptr) *err = "metrics: image row too wide for the kernel";
    return false;
  }
  return true;
}

// 设备是否支持 fp64。SSIM/MS-SSIM 的累加必须在 double 里做（见 metrics.cl），
// 不支持就明确失败，而不是悄悄退化成 float 版本给出一个对不上的数字。
bool deviceSupportsFp64(cl_device_id device) {
  cl_device_fp_config cfg = 0;
  if (clGetDeviceInfo(device, CL_DEVICE_DOUBLE_FP_CONFIG, sizeof(cfg), &cfg,
                      nullptr) != CL_SUCCESS) {
    return false;
  }
  return cfg != 0;
}

// 一次统计所需的 OpenCL 对象。device 由调用方持有。
struct Scratch {
  cl_mem in_a = nullptr;
  cl_mem in_b = nullptr;
  cl_mem win = nullptr;
  cl_mem partials = nullptr;
  size_t band_bytes = 0;
  size_t groups = 0;

  void release() {
    if (in_a != nullptr) clReleaseMemObject(in_a);
    if (in_b != nullptr) clReleaseMemObject(in_b);
    if (win != nullptr) clReleaseMemObject(win);
    if (partials != nullptr) clReleaseMemObject(partials);
    in_a = in_b = win = partials = nullptr;
  }
};

bool ensureScratch(cl_context ctx, Scratch *s, size_t band_bytes, size_t groups,
                   const double *weights, std::string *err) {
  const auto fail = [&](const char *what, cl_int code) {
    if (err != nullptr) {
      *err = std::string("metrics: ") + what + " (" +
             OclProgram::errorName(code) + ")";
    }
    return false;
  };

  // 只增长不缩小：金字塔各级共用一套缓冲，band 尺寸不变时完全无分配。
  if (s->band_bytes < band_bytes) {
    if (s->in_a != nullptr) {
      clReleaseMemObject(s->in_a);
      s->in_a = nullptr;
    }
    if (s->in_b != nullptr) {
      clReleaseMemObject(s->in_b);
      s->in_b = nullptr;
    }

    cl_int code = CL_SUCCESS;
    s->in_a = clCreateBuffer(ctx, CL_MEM_READ_ONLY, band_bytes, nullptr, &code);
    if (code != CL_SUCCESS) return fail("create band buffer a", code);

    s->in_b = clCreateBuffer(ctx, CL_MEM_READ_ONLY, band_bytes, nullptr, &code);
    if (code != CL_SUCCESS) return fail("create band buffer b", code);

    s->band_bytes = band_bytes;
  }

  if (s->win == nullptr) {
    cl_int code = CL_SUCCESS;
    s->win = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                            sizeof(double) * static_cast<size_t>(kWindow),
                            const_cast<double *>(weights), &code);
    if (code != CL_SUCCESS) return fail("create weights buffer", code);
  }

  if (s->groups < groups) {
    if (s->partials != nullptr) clReleaseMemObject(s->partials);
    cl_int code = CL_SUCCESS;
    s->partials = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY,
                                 sizeof(double) * 3 * groups, nullptr, &code);
    if (code != CL_SUCCESS) {
      s->partials = nullptr;
      return fail("create partials buffer", code);
    }
    s->groups = groups;
  }

  return true;
}

// 单尺度统计（GPU）。with_window == 0 时只累加平方误差。
bool scaleStats(const ImageView &a, const ImageView &b, cl_kernel kernel,
                cl_command_queue queue, cl_context ctx, Scratch *scratch,
                int band_rows, int with_window, ScaleStats *out,
                std::string *err) {
  *out = ScaleStats{};

  const int width = a.width;
  const int height = a.height;
  const int channels = a.channels;
  const size_t row_bytes = a.rowBytes();

  const int step = std::clamp(band_rows, 1, height);
  const int rows = step + 2 * kHalo;
  const size_t band_bytes = static_cast<size_t>(rows) * row_bytes;

  const long long tiles = static_cast<long long>(width) * channels;
  long long widest = static_cast<long long>(step) * tiles;
  long long groups = (widest + static_cast<long long>(kLocalSize) - 1) /
                     static_cast<long long>(kLocalSize);
  if (groups < 1) groups = 1;
  if (static_cast<size_t>(groups) > kMaxGroups) groups = kMaxGroups;
  const size_t global_size = static_cast<size_t>(groups) * kLocalSize;

  double weights[kWindow];
  gaussianWeights11(weights);

  if (!ensureScratch(ctx, scratch, band_bytes, static_cast<size_t>(groups),
                     weights, err)) {
    return false;
  }

  std::vector<uint8_t> host_a(band_bytes);
  std::vector<uint8_t> host_b(band_bytes);
  std::vector<double> partials(3 * static_cast<size_t>(groups));

  double sum_ssim = 0.0;
  double sum_cs = 0.0;
  double sum_sq = 0.0;

  for (int y0 = 0; y0 < height; y0 += step) {
    const int expect = std::min(step, height - y0);

    // 越界行在 host 侧复制边缘行后一起上传，kernel 里不再判边界。
    for (int r = 0; r < rows; ++r) {
      int src_row = y0 - kHalo + r;
      if (src_row < 0) src_row = 0;
      if (src_row >= height) src_row = height - 1;

      std::memcpy(host_a.data() + static_cast<size_t>(r) * row_bytes,
                  a.data + static_cast<size_t>(src_row) * row_bytes, row_bytes);
      std::memcpy(host_b.data() + static_cast<size_t>(r) * row_bytes,
                  b.data + static_cast<size_t>(src_row) * row_bytes, row_bytes);
    }

    cl_int code =
        clEnqueueWriteBuffer(queue, scratch->in_a, CL_FALSE, 0, band_bytes,
                             host_a.data(), 0, nullptr, nullptr);
    if (code != CL_SUCCESS) {
      if (err != nullptr) *err = "metrics: upload band a failed";
      return false;
    }
    code = clEnqueueWriteBuffer(queue, scratch->in_b, CL_FALSE, 0, band_bytes,
                                host_b.data(), 0, nullptr, nullptr);
    if (code != CL_SUCCESS) {
      if (err != nullptr) *err = "metrics: upload band b failed";
      return false;
    }

    code = clSetKernelArg(kernel, 0, sizeof(cl_mem), &scratch->in_a);
    code |= clSetKernelArg(kernel, 1, sizeof(cl_mem), &scratch->in_b);
    code |= clSetKernelArg(kernel, 2, sizeof(cl_mem), &scratch->win);
    code |= clSetKernelArg(kernel, 3, sizeof(cl_int), &width);
    code |= clSetKernelArg(kernel, 4, sizeof(cl_int), &height);
    code |= clSetKernelArg(kernel, 5, sizeof(cl_int), &channels);
    code |= clSetKernelArg(kernel, 6, sizeof(cl_int), &y0);
    code |= clSetKernelArg(kernel, 7, sizeof(cl_int), &expect);
    code |= clSetKernelArg(kernel, 8, sizeof(cl_int), &kHalo);
    code |= clSetKernelArg(kernel, 9, sizeof(cl_int), &with_window);
    code |= clSetKernelArg(kernel, 10, sizeof(cl_mem), &scratch->partials);
    if (code != CL_SUCCESS) {
      if (err != nullptr) *err = "metrics: set kernel args failed";
      return false;
    }

    code = clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global_size,
                                  &kLocalSize, 0, nullptr, nullptr);
    if (code != CL_SUCCESS) {
      if (err != nullptr) *err = "metrics: launch kernel failed";
      return false;
    }
    if (clFinish(queue) != CL_SUCCESS) {
      if (err != nullptr) *err = "metrics: kernel finish failed";
      return false;
    }

    code = clEnqueueReadBuffer(queue, scratch->partials, CL_TRUE, 0,
                               partials.size() * sizeof(double),
                               partials.data(), 0, nullptr, nullptr);
    if (code != CL_SUCCESS) {
      if (err != nullptr) *err = "metrics: read partials failed";
      return false;
    }

    for (size_t g = 0; g < static_cast<size_t>(groups); ++g) {
      sum_ssim += partials[g];
      sum_cs += partials[static_cast<size_t>(groups) + g];
      sum_sq += partials[2 * static_cast<size_t>(groups) + g];
    }
  }

  out->ssim_sum = sum_ssim;
  out->cs_sum = sum_cs;
  out->sq_sum = sum_sq;
  out->samples = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) *
                 static_cast<uint64_t>(channels);
  return true;
}

// 建程序 + 取 kernel。每个入口建一次，调用结束即释放：
// 指标的调用频率远低于编解码，缓存程序的收益抵不上生命周期复杂度。
bool buildKernel(cl_context ctx, cl_device_id device, cl_program *program,
                 cl_kernel *kernel, std::string *err) {
  std::string log;
  const cl_int code =
      OclProgram::build(ctx, device, metrics_cl_source,
                        "-cl-std=CL1.2 -cl-mad-enable", program, &log);
  if (code != CL_SUCCESS) {
    if (err != nullptr) {
      *err = "metrics: build program failed: " + log;
    }
    return false;
  }

  if (OclProgram::createKernel(*program, "metrics_band", kernel) !=
      CL_SUCCESS) {
    if (err != nullptr) *err = "metrics: create kernel failed";
    OclProgram::release(*program);
    *program = nullptr;
    return false;
  }
  return true;
}

} // namespace

bool mse(const ImageView &a, const ImageView &b, const Options &options,
         double *out, std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "metrics: null output";
    return false;
  }
  *out = 0.0;

  if (!validate(a, b, err)) return false;
  if (!options.use_gpu) return reference::mse(a, b, out, err);

  OclDevice dev;
  cl_int code = dev.init(options.device_index);
  if (code != CL_SUCCESS) {
    if (err != nullptr) {
      *err = std::string("metrics: no OpenCL device (") +
             OclProgram::errorName(code) + ")";
    }
    return false;
  }
  if (!deviceSupportsFp64(dev.device())) {
    if (err != nullptr) *err = "metrics: device does not support fp64";
    return false;
  }

  cl_program program = nullptr;
  cl_kernel kernel = nullptr;
  if (!buildKernel(dev.context(), dev.device(), &program, &kernel, err)) {
    return false;
  }

  Scratch scratch;
  ScaleStats s;
  const bool ok = scaleStats(a, b, kernel, dev.queue(), dev.context(), &scratch,
                             options.band_rows, /*with_window=*/0, &s, err);

  scratch.release();
  OclProgram::release(kernel);
  OclProgram::release(program);
  if (!ok) return false;

  const double n = static_cast<double>(s.samples);
  if (!(n > 0.0)) {
    if (err != nullptr) *err = "metrics: empty image";
    return false;
  }
  *out = s.sq_sum / n;
  return true;
}

bool psnr(const ImageView &a, const ImageView &b, const Options &options,
          double *out, std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "metrics: null output";
    return false;
  }
  double m = 0.0;
  if (!mse(a, b, options, &m, err)) return false;
  *out = psnrFromMse(m);
  return true;
}

bool ssim(const ImageView &a, const ImageView &b, const Options &options,
          SsimResult *out, std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "metrics: null output";
    return false;
  }
  *out = SsimResult{};

  if (!validate(a, b, err)) return false;
  if (!options.use_gpu) return reference::ssim(a, b, out, err);

  OclDevice dev;
  cl_int code = dev.init(options.device_index);
  if (code != CL_SUCCESS) {
    if (err != nullptr) {
      *err = std::string("metrics: no OpenCL device (") +
             OclProgram::errorName(code) + ")";
    }
    return false;
  }
  if (!deviceSupportsFp64(dev.device())) {
    if (err != nullptr) *err = "metrics: device does not support fp64";
    return false;
  }

  cl_program program = nullptr;
  cl_kernel kernel = nullptr;
  if (!buildKernel(dev.context(), dev.device(), &program, &kernel, err)) {
    return false;
  }

  Scratch scratch;
  bool ok = false;

  // ---- 第 0 级：SSIM 本身 + MS-SSIM 第 0 级的 CS（同一次 11x11 窗统计）----
  ScaleStats s;
  ok = scaleStats(a, b, kernel, dev.queue(), dev.context(), &scratch,
                  options.band_rows, /*with_window=*/1, &s, err);
  if (!ok) {
    scratch.release();
    OclProgram::release(kernel);
    OclProgram::release(program);
    return false;
  }

  const double n0 = static_cast<double>(s.samples);
  if (!(n0 > 0.0)) {
    if (err != nullptr) *err = "metrics: empty image";
    scratch.release();
    OclProgram::release(kernel);
    OclProgram::release(program);
    return false;
  }
  out->ssim = s.ssim_sum / n0;

  // ---- MS-SSIM：逐级 2x2 均值下采样，只有最末一级取完整 SSIM ----
  const int levels = msSsimLevels(a.width, a.height);
  out->levels = levels;

  double cs[kMsSsimMaxLevels] = {0.0, 0.0, 0.0, 0.0, 0.0};
  cs[0] = s.cs_sum / n0;

  double last_ssim = out->ssim;

  // 层级间只保留当前层与下一层：整座金字塔在大图上要白占几百 MB 宿主内存。
  std::vector<uint8_t> cur_a;
  std::vector<uint8_t> cur_b;
  std::vector<uint8_t> next_a;
  std::vector<uint8_t> next_b;
  int cur_w = a.width;
  int cur_h = a.height;

  for (int level = 1; level < levels; ++level) {
    ImageView src_a;
    ImageView src_b;

    if (level == 1) {
      src_a = a;
      src_b = b;
    } else {
      src_a.data = cur_a.data();
      src_a.width = cur_w;
      src_a.height = cur_h;
      src_a.channels = a.channels;
      src_b.data = cur_b.data();
      src_b.width = cur_w;
      src_b.height = cur_h;
      src_b.channels = a.channels;
    }

    int nw = 0;
    int nh = 0;
    if (!downsample2x(src_a, &next_a, &nw, &nh) ||
        !downsample2x(src_b, &next_b, &nw, &nh)) {
      if (err != nullptr) *err = "metrics: pyramid downsampling failed";
      scratch.release();
      OclProgram::release(kernel);
      OclProgram::release(program);
      return false;
    }

    ImageView la;
    la.data = next_a.data();
    la.width = nw;
    la.height = nh;
    la.channels = a.channels;

    ImageView lb;
    lb.data = next_b.data();
    lb.width = nw;
    lb.height = nh;
    lb.channels = a.channels;

    ScaleStats sl;
    if (!scaleStats(la, lb, kernel, dev.queue(), dev.context(), &scratch,
                    options.band_rows, /*with_window=*/1, &sl, err)) {
      scratch.release();
      OclProgram::release(kernel);
      OclProgram::release(program);
      return false;
    }

    const double nl = static_cast<double>(sl.samples);
    cs[level] = sl.cs_sum / nl;
    last_ssim = sl.ssim_sum / nl;

    cur_a = std::move(next_a);
    cur_b = std::move(next_b);
    cur_w = nw;
    cur_h = nh;
  }

  out->ms_ssim = msSsimFromScales(cs, last_ssim, levels);

  scratch.release();
  OclProgram::release(kernel);
  OclProgram::release(program);
  return true;
}

} // namespace metrics
} // namespace ocl
} // namespace image_codec
