/*
 * File: ocl_program.cpp
 * Brief: Ocl program.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#include "ocl_program.h"

#include <cstring>

namespace image_codec {

cl_int OclProgram::build(cl_context ctx, cl_device_id dev, const char *source,
                         const char *options, cl_program *out_program,
                         std::string *build_log) {
  if (ctx == nullptr || dev == nullptr || source == nullptr || out_program == nullptr) {
    return CL_INVALID_VALUE;
  }
  cl_int err = CL_SUCCESS;
  const size_t length = std::strlen(source);
  cl_program program = clCreateProgramWithSource(ctx, 1, &source, &length, &err);
  if (err != CL_SUCCESS || program == nullptr) {
    return err != CL_SUCCESS ? err : CL_INVALID_PROGRAM;
  }
  err = clBuildProgram(program, 1, &dev, options, nullptr, nullptr);
  if (err != CL_SUCCESS && build_log != nullptr) {
    size_t logLen = 0;
    clGetProgramBuildInfo(program, dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &logLen);
    if (logLen > 0) {
      std::string log(logLen, '\0');
      clGetProgramBuildInfo(program, dev, CL_PROGRAM_BUILD_LOG, logLen, &log[0], nullptr);
      *build_log = log;
    }
  }
  if (err != CL_SUCCESS) {
    clReleaseProgram(program);
    return err;
  }
  *out_program = program;
  return CL_SUCCESS;
}

cl_int OclProgram::createKernel(cl_program program, const char *name,
                                cl_kernel *out_kernel) {
  if (program == nullptr || name == nullptr || out_kernel == nullptr) {
    return CL_INVALID_VALUE;
  }
  cl_int err = CL_SUCCESS;
  cl_kernel kernel = clCreateKernel(program, name, &err);
  if (err != CL_SUCCESS) {
    return err;
  }
  *out_kernel = kernel;
  return CL_SUCCESS;
}

void OclProgram::release(cl_program p) {
  if (p != nullptr) {
    clReleaseProgram(p);
  }
}

void OclProgram::release(cl_kernel k) {
  if (k != nullptr) {
    clReleaseKernel(k);
  }
}

const char *OclProgram::errorName(cl_int err) {
  switch (err) {
    case CL_SUCCESS: return "CL_SUCCESS";
    case CL_DEVICE_NOT_FOUND: return "CL_DEVICE_NOT_FOUND";
    case CL_DEVICE_NOT_AVAILABLE: return "CL_DEVICE_NOT_AVAILABLE";
    case CL_COMPILER_NOT_AVAILABLE: return "CL_COMPILER_NOT_AVAILABLE";
    case CL_MEM_OBJECT_ALLOCATION_FAILURE: return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
    case CL_OUT_OF_RESOURCES: return "CL_OUT_OF_RESOURCES";
    case CL_OUT_OF_HOST_MEMORY: return "CL_OUT_OF_HOST_MEMORY";
    case CL_MEM_COPY_OVERLAP: return "CL_MEM_COPY_OVERLAP";
    case CL_BUILD_PROGRAM_FAILURE: return "CL_BUILD_PROGRAM_FAILURE";
    case CL_MAP_FAILURE: return "CL_MAP_FAILURE";
    case CL_INVALID_VALUE: return "CL_INVALID_VALUE";
    case CL_INVALID_DEVICE_TYPE: return "CL_INVALID_DEVICE_TYPE";
    case CL_INVALID_PLATFORM: return "CL_INVALID_PLATFORM";
    case CL_INVALID_DEVICE: return "CL_INVALID_DEVICE";
    case CL_INVALID_CONTEXT: return "CL_INVALID_CONTEXT";
    case CL_INVALID_QUEUE_PROPERTIES: return "CL_INVALID_QUEUE_PROPERTIES";
    case CL_INVALID_COMMAND_QUEUE: return "CL_INVALID_COMMAND_QUEUE";
    case CL_INVALID_HOST_PTR: return "CL_INVALID_HOST_PTR";
    case CL_INVALID_MEM_OBJECT: return "CL_INVALID_MEM_OBJECT";
    case CL_INVALID_IMAGE_FORMAT_DESCRIPTOR: return "CL_INVALID_IMAGE_FORMAT_DESCRIPTOR";
    case CL_INVALID_IMAGE_SIZE: return "CL_INVALID_IMAGE_SIZE";
    case CL_INVALID_SAMPLER: return "CL_INVALID_SAMPLER";
    case CL_INVALID_BINARY: return "CL_INVALID_BINARY";
    case CL_INVALID_BUILD_OPTIONS: return "CL_INVALID_BUILD_OPTIONS";
    case CL_INVALID_PROGRAM: return "CL_INVALID_PROGRAM";
    case CL_INVALID_PROGRAM_EXECUTABLE: return "CL_INVALID_PROGRAM_EXECUTABLE";
    case CL_INVALID_KERNEL_NAME: return "CL_INVALID_KERNEL_NAME";
    case CL_INVALID_KERNEL_DEFINITION: return "CL_INVALID_KERNEL_DEFINITION";
    case CL_INVALID_KERNEL: return "CL_INVALID_KERNEL";
    case CL_INVALID_ARG_INDEX: return "CL_INVALID_ARG_INDEX";
    case CL_INVALID_ARG_VALUE: return "CL_INVALID_ARG_VALUE";
    case CL_INVALID_ARG_SIZE: return "CL_INVALID_ARG_SIZE";
    case CL_INVALID_KERNEL_ARGS: return "CL_INVALID_KERNEL_ARGS";
    case CL_INVALID_WORK_DIMENSION: return "CL_INVALID_WORK_DIMENSION";
    case CL_INVALID_WORK_GROUP_SIZE: return "CL_INVALID_WORK_GROUP_SIZE";
    case CL_INVALID_WORK_ITEM_SIZE: return "CL_INVALID_WORK_ITEM_SIZE";
    case CL_INVALID_GLOBAL_OFFSET: return "CL_INVALID_GLOBAL_OFFSET";
    case CL_INVALID_EVENT_WAIT_LIST: return "CL_INVALID_EVENT_WAIT_LIST";
    case CL_INVALID_EVENT: return "CL_INVALID_EVENT";
    case CL_INVALID_OPERATION: return "CL_INVALID_OPERATION";
    case CL_INVALID_GL_OBJECT: return "CL_INVALID_GL_OBJECT";
    case CL_INVALID_BUFFER_SIZE: return "CL_INVALID_BUFFER_SIZE";
    case CL_INVALID_MIP_LEVEL: return "CL_INVALID_MIP_LEVEL";
    case CL_INVALID_GLOBAL_WORK_SIZE: return "CL_INVALID_GLOBAL_WORK_SIZE";
    case CL_INVALID_PROPERTY: return "CL_INVALID_PROPERTY";
    default: return "UNKNOWN_OPENCL_ERROR";
  }
}

}  // namespace image_codec
