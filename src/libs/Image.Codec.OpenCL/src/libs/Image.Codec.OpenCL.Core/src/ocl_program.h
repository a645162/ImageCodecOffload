/*
 * File: ocl_program.h
 * Brief: Ocl program.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// opencl/ocl_program.h - 从 OpenCL C 源码构建 program/kernel 的工具。
#pragma once

#include <CL/cl.h>

#include <string>

namespace image_codec {

struct OclProgram {
  // 从源码字符串构建 program（失败返回 OpenCL 错误码，build_log 含诊断）。
  static cl_int build(cl_context ctx, cl_device_id dev, const char *source,
                      const char *options, cl_program *out_program,
                      std::string *build_log = nullptr);
  static cl_int createKernel(cl_program program, const char *name,
                             cl_kernel *out_kernel);
  static void release(cl_program p);
  static void release(cl_kernel k);
  // 将 OpenCL 错误码转为可读名称（诊断用）。
  static const char *errorName(cl_int err);
};

}  // namespace image_codec
