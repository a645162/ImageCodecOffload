/*
 * File: lzw_decode_ocl.cpp
 * Brief: Lzw decode ocl.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/lzw_decode_ocl.cpp - GPU LZW 解码（段级并行探索实现，见 lzw_decode_ocl.h）。
//
// 流程：段扫描(host, 串行, 找 ClearCode 段边界并校验码流) -> 两遍 kernel
// （mode0 测各段输出长度 -> mode1 按偏移写输出）。输出与 CPU lzwDecode 逐字节一致。
#include "lzw_decode_ocl.h"

#include "ocl_device.h"
#include "ocl_program.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 内嵌 kernel 源码（由 CMake file(READ)+configure_file 生成）。
namespace image_codec {
namespace tiff {
extern const char *lzw_decode_cl_source;
extern const char *lzw_decode_par_cl_source;
} // namespace tiff
} // namespace image_codec

namespace image_codec {
namespace tiff {

namespace {

constexpr uint32_t kClear = 256;
constexpr uint32_t kEoi = 257;
constexpr uint32_t kFirst = 258;
constexpr uint32_t kMaxEnt = 4096;
constexpr uint32_t kMaxInBytes = 1u << 28; // 单流上限（bit 偏移用 uint32）

// 段：起始/结束 bit（不含终止码）。bit 0 为 in[0] 的最高位（MSB-first）。
struct Segment {
  uint32_t start_bit;
  uint32_t end_bit;
  uint32_t n_codes; // 段内码字数（不含 ClearCode/EOI 终止码）
};

// 段内第 j 个码的码宽（位置公式，等价于解码器 next_code==511/1023/2047 切换）。
inline uint32_t segWidth(uint32_t j) {
  if (j <= 253) return 9;
  if (j <= 765) return 10;
  if (j <= 1789) return 11;
  return 12;
}

// 读取 MSB-first 变长码（3 字节窗口，in 需带尾部 padding）。
inline uint32_t readCode(const std::vector<uint8_t> &in, uint32_t bitpos,
                         uint32_t w) {
  const uint32_t byte = bitpos >> 3;
  const uint32_t bit = bitpos & 7;
  const uint32_t concat = (static_cast<uint32_t>(in[byte]) << 16) |
                          (static_cast<uint32_t>(in[byte + 1]) << 8) |
                          static_cast<uint32_t>(in[byte + 2]);
  return (concat >> (24 - bit - w)) & ((1u << w) - 1u);
}

// 段扫描：顺序读码，找 ClearCode/EOI 段边界并校验码流。
// 返回 0 成功；负值与 CPU lzwDecode 错误码一致（-2/-3/-4/-5）。
int scanSegments(const uint8_t *in, size_t in_size,
                 std::vector<Segment> &segs) {
  segs.clear();
  if (in == nullptr && in_size > 0) return -1;
  if (in_size > kMaxInBytes) return -2; // 超出本实现上限

  std::vector<uint8_t> padded(in_size + 3, 0);
  if (in_size > 0) std::memcpy(padded.data(), in, in_size);

  uint32_t bitpos = 0;
  uint32_t j = 0;         // 段内码序号
  uint32_t seg_start = 0; // 当前段起始 bit
  uint32_t next_code = kFirst;
  int old = -1;

  for (;;) {
    if (bitpos >> 3 >= in_size) return -2; // 位流提前结束（缺 EOI）
    const uint32_t w = segWidth(j);
    if (static_cast<uint64_t>(bitpos) + w >
        static_cast<uint64_t>(in_size) * 8) {
      return -2; // 截断的码
    }
    const uint32_t code = readCode(padded, bitpos, w);
    bitpos += w;
    j++;

    if (code == kEoi) {
      segs.push_back({seg_start, bitpos - w, j - 1});
      return 0;
    }
    if (code == kClear) {
      segs.push_back(
          {seg_start, bitpos - w, j - 1}); // 本段结束于该 Clear 的 bit 起点
      seg_start = bitpos;                  // 下一段从 Clear 之后开始
      next_code = kFirst;
      j = 0;
      old = -1;
      continue;
    }
    if (code >= kMaxEnt) return -3;
    if (old == -1) {
      if (code >= 256) return -4; // 段首必须为字面量
      old = static_cast<int>(code);
      continue;
    }
    if (code > next_code) return -5;      // 非法码（code > next_code）
    if (next_code < kMaxEnt) next_code++; // 每码加一个条目
    old = static_cast<int>(code);
  }
}

// 组装错误信息并打印一行诊断。
std::string makeErr(const char *what, cl_int err, const std::string &log) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%s: %s", what, OclProgram::errorName(err));
  std::string s = buf;
  if (!log.empty()) s += "\n[build log]\n" + log;
  return s;
}

// 单次 kernel 入参集合。
struct KernArgs {
  cl_mem in = nullptr;
  cl_mem prefix = nullptr;
  cl_mem suffix = nullptr;
  cl_mem stack = nullptr;
  cl_mem out = nullptr;
  cl_mem seg_start = nullptr;
  cl_mem seg_end = nullptr;
  cl_mem seg_out_offset = nullptr;
  cl_mem seg_len = nullptr;
  cl_mem seg_status = nullptr;
};

void releaseBuffers(KernArgs &a) {
  if (a.in) clReleaseMemObject(a.in);
  if (a.prefix) clReleaseMemObject(a.prefix);
  if (a.suffix) clReleaseMemObject(a.suffix);
  if (a.stack) clReleaseMemObject(a.stack);
  if (a.out) clReleaseMemObject(a.out);
  if (a.seg_start) clReleaseMemObject(a.seg_start);
  if (a.seg_end) clReleaseMemObject(a.seg_end);
  if (a.seg_out_offset) clReleaseMemObject(a.seg_out_offset);
  if (a.seg_len) clReleaseMemObject(a.seg_len);
  if (a.seg_status) clReleaseMemObject(a.seg_status);
  a = KernArgs{};
}

// 段内并行解码（gpuLzwDecodePar）的 kernel 入参集合。
struct ParArgs {
  cl_mem in = nullptr;
  cl_mem seg_start = nullptr;
  cl_mem seg_ncodes = nullptr;
  cl_mem codes = nullptr;
  cl_mem suffix = nullptr;
  cl_mem lengths = nullptr;
  cl_mem offsets = nullptr;
  cl_mem out_base = nullptr;
  cl_mem status = nullptr;
  cl_mem out = nullptr;
};

void releaseParBuffers(ParArgs &a) {
  if (a.in) clReleaseMemObject(a.in);
  if (a.seg_start) clReleaseMemObject(a.seg_start);
  if (a.seg_ncodes) clReleaseMemObject(a.seg_ncodes);
  if (a.codes) clReleaseMemObject(a.codes);
  if (a.suffix) clReleaseMemObject(a.suffix);
  if (a.lengths) clReleaseMemObject(a.lengths);
  if (a.offsets) clReleaseMemObject(a.offsets);
  if (a.out_base) clReleaseMemObject(a.out_base);
  if (a.status) clReleaseMemObject(a.status);
  if (a.out) clReleaseMemObject(a.out);
  a = ParArgs{};
}

} // namespace

int gpuLzwDecode(const uint8_t *in, size_t in_size, std::vector<uint8_t> &out) {
  out.clear();

  // ---- 段扫描（host，串行）：找段边界并校验 ----
  std::vector<Segment> segs;
  int rc = scanSegments(in, in_size, segs);
  if (rc != 0) return rc;

  // 过滤空段（连续 ClearCode / 流首 ClearCode 会产生空段）。
  std::vector<Segment> real;
  real.reserve(segs.size());
  for (const Segment &s : segs) {
    if (s.start_bit < s.end_bit) real.push_back(s);
  }
  const size_t n_seg = real.size();
  if (n_seg == 0) return 0; // 无有效数据（如 ClearCode+EOI 空流）

  // ---- 设备/程序初始化 ----
  OclDevice dev;
  cl_int err = dev.init(0);
  if (err != CL_SUCCESS) return -10;
  cl_context ctx = dev.context();
  cl_command_queue queue = dev.queue();
  cl_device_id device_id = dev.device();

  std::string log;
  cl_program program = nullptr;
  err = OclProgram::build(ctx, device_id, lzw_decode_cl_source, "-cl-std=CL1.2",
                          &program, &log);
  if (err != CL_SUCCESS) return -11;
  cl_kernel kernel = nullptr;
  err = OclProgram::createKernel(program, "lzw_decode", &kernel);
  if (err != CL_SUCCESS) {
    OclProgram::release(program);
    return -12;
  }

  // ---- 缓冲 ----
  std::vector<uint8_t> padded(in_size + 3, 0);
  if (in_size > 0) std::memcpy(padded.data(), in, in_size);

  std::vector<uint32_t> starts(n_seg), ends(n_seg);
  for (size_t i = 0; i < n_seg; ++i) {
    starts[i] = real[i].start_bit;
    ends[i] = real[i].end_bit;
  }

  // 提前声明：MSVC C2362 禁止 goto cleanup 跳过带初始化的变量声明。
  std::vector<cl_uint> lens;
  std::vector<cl_int> status;
  std::vector<cl_uint> offsets;

  KernArgs a;
  a.in = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                        padded.size(), padded.data(), &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.prefix = clCreateBuffer(ctx, CL_MEM_READ_WRITE,
                            n_seg * 4096 * sizeof(cl_ushort), nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.suffix =
      clCreateBuffer(ctx, CL_MEM_READ_WRITE, n_seg * 4096, nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.stack = clCreateBuffer(ctx, CL_MEM_READ_WRITE, n_seg * 4128, nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.seg_start = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                               n_seg * sizeof(uint32_t), starts.data(), &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.seg_end = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                             n_seg * sizeof(uint32_t), ends.data(), &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.seg_len = clCreateBuffer(ctx, CL_MEM_READ_WRITE, n_seg * sizeof(cl_uint),
                             nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.seg_out_offset = clCreateBuffer(ctx, CL_MEM_READ_WRITE,
                                    n_seg * sizeof(cl_uint), nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.seg_status = clCreateBuffer(ctx, CL_MEM_READ_WRITE, n_seg * sizeof(cl_int),
                                nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }

  {
    const cl_uint zero32 = 0;
    clEnqueueFillBuffer(queue, a.seg_len, &zero32, sizeof(zero32), 0,
                        n_seg * sizeof(cl_uint), 0, nullptr, nullptr);
    const cl_int zeroI = 0;
    clEnqueueFillBuffer(queue, a.seg_status, &zeroI, sizeof(zeroI), 0,
                        n_seg * sizeof(cl_int), 0, nullptr, nullptr);
  }

  // 从这里到 rc = 0 的整段放进一个作用域块：块内声明了带初始化的变量
  // （c_nseg / gws / out_null ...），而上面的错误分支要 goto 到块外的 cleanup。
  // C++ 不允许 goto 跳过带初始化的声明，MSVC(C2362) 与 gcc 都会报错。
  {
    const cl_uint c_nseg = static_cast<cl_uint>(n_seg);

    // ---- 第 1 遍：测各段输出长度 ----
    {
      clSetKernelArg(kernel, 0, sizeof(cl_mem), &a.in);
      clSetKernelArg(kernel, 1, sizeof(cl_mem), &a.prefix);
      clSetKernelArg(kernel, 2, sizeof(cl_mem), &a.suffix);
      clSetKernelArg(kernel, 3, sizeof(cl_mem), &a.stack);
      cl_mem out_null = nullptr; // mode0 不写 out
      clSetKernelArg(kernel, 4, sizeof(cl_mem), &out_null);
      clSetKernelArg(kernel, 5, sizeof(cl_mem), &a.seg_start);
      clSetKernelArg(kernel, 6, sizeof(cl_mem), &a.seg_end);
      clSetKernelArg(kernel, 7, sizeof(cl_mem), &a.seg_out_offset);
      clSetKernelArg(kernel, 8, sizeof(cl_mem), &a.seg_len);
      clSetKernelArg(kernel, 9, sizeof(cl_mem), &a.seg_status);
      clSetKernelArg(kernel, 10, sizeof(cl_uint), &c_nseg);
      const cl_int mode0 = 0;
      clSetKernelArg(kernel, 11, sizeof(cl_int), &mode0);
      const size_t gws = n_seg;
      err = clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &gws, nullptr, 0,
                                   nullptr, nullptr);
      if (err != CL_SUCCESS) {
        rc = -14;
        goto cleanup;
      }
      if (clFinish(queue) != CL_SUCCESS) {
        rc = -14;
        goto cleanup;
      }
    }

    // ---- 读回长度与状态 ----
    lens.resize(n_seg);
    if (clEnqueueReadBuffer(queue, a.seg_len, CL_TRUE, 0,
                            n_seg * sizeof(cl_uint), lens.data(), 0, nullptr,
                            nullptr) != CL_SUCCESS) {
      rc = -15;
      goto cleanup;
    }
    status.resize(n_seg);
    if (clEnqueueReadBuffer(queue, a.seg_status, CL_TRUE, 0,
                            n_seg * sizeof(cl_int), status.data(), 0, nullptr,
                            nullptr) != CL_SUCCESS) {
      rc = -15;
      goto cleanup;
    }
    for (size_t i = 0; i < n_seg; ++i) {
      if (status[i] != 0) {
        rc = status[i]; // 与 CPU 错误码一致的负值
        goto cleanup;
      }
    }

    uint64_t total = 0;
    offsets.resize(n_seg);
    for (size_t i = 0; i < n_seg; ++i) {
      offsets[i] = static_cast<cl_uint>(total);
      total += lens[i];
    }
    if (total == 0) {
      rc = 0;
      goto cleanup;
    }

    // ---- 第 2 遍：按偏移写输出 ----
    {
      a.out = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, static_cast<size_t>(total),
                             nullptr, &err);
      if (err != CL_SUCCESS) {
        rc = -13;
        goto cleanup;
      }
      if (clEnqueueWriteBuffer(queue, a.seg_out_offset, CL_TRUE, 0,
                               n_seg * sizeof(cl_uint), offsets.data(), 0,
                               nullptr, nullptr) != CL_SUCCESS) {
        rc = -15;
        goto cleanup;
      }
      clSetKernelArg(kernel, 0, sizeof(cl_mem), &a.in);
      clSetKernelArg(kernel, 1, sizeof(cl_mem), &a.prefix);
      clSetKernelArg(kernel, 2, sizeof(cl_mem), &a.suffix);
      clSetKernelArg(kernel, 3, sizeof(cl_mem), &a.stack);
      clSetKernelArg(kernel, 4, sizeof(cl_mem), &a.out);
      clSetKernelArg(kernel, 5, sizeof(cl_mem), &a.seg_start);
      clSetKernelArg(kernel, 6, sizeof(cl_mem), &a.seg_end);
      clSetKernelArg(kernel, 7, sizeof(cl_mem), &a.seg_out_offset);
      clSetKernelArg(kernel, 8, sizeof(cl_mem), &a.seg_len);
      clSetKernelArg(kernel, 9, sizeof(cl_mem), &a.seg_status);
      clSetKernelArg(kernel, 10, sizeof(cl_uint), &c_nseg);
      const cl_int mode1 = 1;
      clSetKernelArg(kernel, 11, sizeof(cl_int), &mode1);
      const size_t gws = n_seg;
      err = clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &gws, nullptr, 0,
                                   nullptr, nullptr);
      if (err != CL_SUCCESS) {
        rc = -14;
        goto cleanup;
      }
      if (clFinish(queue) != CL_SUCCESS) {
        rc = -14;
        goto cleanup;
      }
    }

    // ---- 读回输出 ----
    out.resize(static_cast<size_t>(total));
    if (clEnqueueReadBuffer(queue, a.out, CL_TRUE, 0, out.size(), out.data(), 0,
                            nullptr, nullptr) != CL_SUCCESS) {
      rc = -15;
      goto cleanup;
    }
    rc = 0;
  }

cleanup:
  releaseBuffers(a);
  OclProgram::release(kernel);
  OclProgram::release(program);
  if (rc != 0) out.clear();
  return rc;
}

int gpuLzwDecodePar(const uint8_t *in, size_t in_size,
                    std::vector<uint8_t> &out) {
  out.clear();

  // ---- 段扫描（host，串行）：找段边界 + 校验码流（复用段级并行版的扫描） ----
  std::vector<Segment> segs;
  int rc = scanSegments(in, in_size, segs);
  if (rc != 0) return rc;

  // 过滤空段（连续 ClearCode / 流首 ClearCode 会产生空段）。
  std::vector<Segment> real;
  real.reserve(segs.size());
  for (const Segment &s : segs) {
    if (s.start_bit < s.end_bit) real.push_back(s);
  }
  const size_t n_seg = real.size();
  if (n_seg == 0) return 0; // 无有效数据（如 ClearCode+EOI 空流）

  uint32_t max_ncodes = 1;
  for (const Segment &s : real) max_ncodes = std::max(max_ncodes, s.n_codes);

  // ---- 设备/程序初始化 ----
  OclDevice dev;
  cl_int err = dev.init(0);
  if (err != CL_SUCCESS) return -10;
  cl_context ctx = dev.context();
  cl_command_queue queue = dev.queue();
  cl_device_id device_id = dev.device();

  std::string log;
  cl_program program = nullptr;
  err = OclProgram::build(ctx, device_id, lzw_decode_par_cl_source,
                          "-cl-std=CL1.2", &program, &log);
  if (err != CL_SUCCESS) return -11;
  cl_kernel k_extract = nullptr, k_build = nullptr, k_write = nullptr;
  err = OclProgram::createKernel(program, "lzw_par_extract", &k_extract);
  if (err != CL_SUCCESS) {
    OclProgram::release(program);
    return -12;
  }
  err = OclProgram::createKernel(program, "lzw_par_build", &k_build);
  if (err != CL_SUCCESS) {
    OclProgram::release(k_extract);
    OclProgram::release(program);
    return -12;
  }
  err = OclProgram::createKernel(program, "lzw_par_write", &k_write);
  if (err != CL_SUCCESS) {
    OclProgram::release(k_build);
    OclProgram::release(k_extract);
    OclProgram::release(program);
    return -12;
  }

  // ---- 缓冲 ----
  std::vector<uint8_t> padded(in_size + 3, 0);
  if (in_size > 0) std::memcpy(padded.data(), in, in_size);

  std::vector<uint32_t> starts(n_seg), ncodes(n_seg);
  for (size_t i = 0; i < n_seg; ++i) {
    starts[i] = real[i].start_bit;
    ncodes[i] = real[i].n_codes;
  }

  // 提前声明（MSVC C2362 禁止 goto 跳过带初始化的变量声明）。
  std::vector<cl_uint> lens;
  std::vector<cl_int> status;
  std::vector<cl_uint> offsets, bases;

  ParArgs a;
  a.in = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                        padded.size(), padded.data(), &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.seg_start = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                               n_seg * sizeof(uint32_t), starts.data(), &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.seg_ncodes = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                n_seg * sizeof(uint32_t), ncodes.data(), &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.codes = clCreateBuffer(ctx, CL_MEM_READ_WRITE,
                           n_seg * 4096 * sizeof(cl_uint), nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.suffix =
      clCreateBuffer(ctx, CL_MEM_READ_WRITE, n_seg * 4096, nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.lengths = clCreateBuffer(ctx, CL_MEM_READ_WRITE,
                             n_seg * 4096 * sizeof(cl_uint), nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.offsets = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY,
                             n_seg * 4096 * sizeof(cl_uint), nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.out_base = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, n_seg * sizeof(cl_uint),
                              nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }
  a.status = clCreateBuffer(ctx, CL_MEM_READ_WRITE, n_seg * sizeof(cl_int),
                            nullptr, &err);
  if (err != CL_SUCCESS) {
    rc = -13;
    goto cleanup;
  }

  // 同上：块内变量带初始化，错误分支 goto 到块外的 cleanup。
  {
    const cl_uint c_nseg = static_cast<cl_uint>(n_seg);
    const size_t gws[2] = {max_ncodes, n_seg};

    // ---- kernel 1：并行读码 + 校验 ----
    {
      clSetKernelArg(k_extract, 0, sizeof(cl_mem), &a.in);
      clSetKernelArg(k_extract, 1, sizeof(cl_mem), &a.seg_start);
      clSetKernelArg(k_extract, 2, sizeof(cl_mem), &a.seg_ncodes);
      clSetKernelArg(k_extract, 3, sizeof(cl_mem), &a.codes);
      clSetKernelArg(k_extract, 4, sizeof(cl_mem), &a.status);
      clSetKernelArg(k_extract, 5, sizeof(cl_uint), &c_nseg);
      err = clEnqueueNDRangeKernel(queue, k_extract, 2, nullptr, gws, nullptr,
                                   0, nullptr, nullptr);
      if (err != CL_SUCCESS) {
        rc = -14;
        goto cleanup;
      }
      if (clFinish(queue) != CL_SUCCESS) {
        rc = -14;
        goto cleanup;
      }
    }

    // ---- 读回状态；有错误则中止 ----
    status.resize(n_seg);
    if (clEnqueueReadBuffer(queue, a.status, CL_TRUE, 0, n_seg * sizeof(cl_int),
                            status.data(), 0, nullptr, nullptr) != CL_SUCCESS) {
      rc = -15;
      goto cleanup;
    }
    for (size_t i = 0; i < n_seg; ++i) {
      if (status[i] != 0) {
        rc = status[i]; // 与 CPU 参考错误码一致的负值
        goto cleanup;
      }
    }

    // ---- kernel 2：并行建字典（suffix）+ 求输出长度 ----
    {
      clSetKernelArg(k_build, 0, sizeof(cl_mem), &a.codes);
      clSetKernelArg(k_build, 1, sizeof(cl_mem), &a.suffix);
      clSetKernelArg(k_build, 2, sizeof(cl_mem), &a.lengths);
      clSetKernelArg(k_build, 3, sizeof(cl_mem), &a.seg_ncodes);
      clSetKernelArg(k_build, 4, sizeof(cl_uint), &c_nseg);
      err = clEnqueueNDRangeKernel(queue, k_build, 2, nullptr, gws, nullptr, 0,
                                   nullptr, nullptr);
      if (err != CL_SUCCESS) {
        rc = -14;
        goto cleanup;
      }
      if (clFinish(queue) != CL_SUCCESS) {
        rc = -14;
        goto cleanup;
      }
    }

    // ---- 读回长度，host 求每码字段内偏移与段全局偏移 ----
    lens.resize(n_seg * 4096);
    if (clEnqueueReadBuffer(queue, a.lengths, CL_TRUE, 0,
                            lens.size() * sizeof(cl_uint), lens.data(), 0,
                            nullptr, nullptr) != CL_SUCCESS) {
      rc = -15;
      goto cleanup;
    }
    offsets.assign(n_seg * 4096, 0u);
    bases.resize(n_seg);
    uint64_t total = 0;
    for (size_t s = 0; s < n_seg; ++s) {
      bases[s] = static_cast<cl_uint>(total);
      uint64_t acc = 0;
      for (uint32_t k = 0; k < real[s].n_codes; ++k) {
        offsets[s * 4096 + k] = static_cast<cl_uint>(acc);
        acc += lens[s * 4096 + k];
      }
      total += acc;
    }
    if (total == 0) {
      rc = 0;
      goto cleanup;
    }

    // ---- 写回偏移/段基址，kernel 3：并行展开输出 ----
    {
      if (clEnqueueWriteBuffer(queue, a.offsets, CL_TRUE, 0,
                               offsets.size() * sizeof(cl_uint), offsets.data(),
                               0, nullptr, nullptr) != CL_SUCCESS) {
        rc = -15;
        goto cleanup;
      }
      if (clEnqueueWriteBuffer(queue, a.out_base, CL_TRUE, 0,
                               bases.size() * sizeof(cl_uint), bases.data(), 0,
                               nullptr, nullptr) != CL_SUCCESS) {
        rc = -15;
        goto cleanup;
      }
      a.out = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, static_cast<size_t>(total),
                             nullptr, &err);
      if (err != CL_SUCCESS) {
        rc = -13;
        goto cleanup;
      }
      clSetKernelArg(k_write, 0, sizeof(cl_mem), &a.codes);
      clSetKernelArg(k_write, 1, sizeof(cl_mem), &a.suffix);
      clSetKernelArg(k_write, 2, sizeof(cl_mem), &a.lengths);
      clSetKernelArg(k_write, 3, sizeof(cl_mem), &a.offsets);
      clSetKernelArg(k_write, 4, sizeof(cl_mem), &a.out_base);
      clSetKernelArg(k_write, 5, sizeof(cl_mem), &a.out);
      clSetKernelArg(k_write, 6, sizeof(cl_mem), &a.seg_ncodes);
      clSetKernelArg(k_write, 7, sizeof(cl_uint), &c_nseg);
      err = clEnqueueNDRangeKernel(queue, k_write, 2, nullptr, gws, nullptr, 0,
                                   nullptr, nullptr);
      if (err != CL_SUCCESS) {
        rc = -14;
        goto cleanup;
      }
      if (clFinish(queue) != CL_SUCCESS) {
        rc = -14;
        goto cleanup;
      }
    }

    // ---- 读回输出 ----
    out.resize(static_cast<size_t>(total));
    if (clEnqueueReadBuffer(queue, a.out, CL_TRUE, 0, out.size(), out.data(), 0,
                            nullptr, nullptr) != CL_SUCCESS) {
      rc = -15;
      goto cleanup;
    }
    rc = 0;
  }

cleanup:
  releaseParBuffers(a);
  OclProgram::release(k_write);
  OclProgram::release(k_build);
  OclProgram::release(k_extract);
  OclProgram::release(program);
  if (rc != 0) out.clear();
  return rc;
}

} // namespace tiff
} // namespace image_codec
