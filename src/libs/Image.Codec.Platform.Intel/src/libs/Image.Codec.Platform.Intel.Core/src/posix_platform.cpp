/*
 * File: posix_platform.cpp
 * Brief: Posix platform.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// posix_platform.cpp - POSIX（Linux/macOS）平台抽象实现。
//
// 可执行目录用 /proc/self/exe 解析（Linux）；其它 POSIX 系统回退到
// _NSGetExecutablePath / /proc/curproc/file，都不支持时返回 nullptr，
// 由调用方按「拿不到 exe 目录」处理，而不是猜一个当前工作目录。
#include "oic_platform.h"

#if !defined(_WIN32)
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

char *duplicate_string(const char *text) {
  if (text == nullptr) {
    return nullptr;
  }
  const size_t len = std::strlen(text);
  char *out = static_cast<char *>(std::malloc(len + 1));
  if (out == nullptr) {
    return nullptr;
  }
  std::memcpy(out, text, len + 1);
  return out;
}

bool read_self_exe(char *buffer, size_t capacity) {
#if defined(__linux__)
  const ssize_t n = ::readlink("/proc/self/exe", buffer, capacity - 1);
  if (n <= 0) {
    return false;
  }
  buffer[n] = '\0';
  return true;
#elif defined(__APPLE__)
  uint32_t size = static_cast<uint32_t>(capacity);
  if (_NSGetExecutablePath(buffer, &size) != 0) {
    return false;
  }
  buffer[capacity - 1] = '\0';
  return true;
#else
  (void)buffer;
  (void)capacity;
  return false;
#endif
}

} // namespace

char *oic_exe_dir(void) {
  char path[4096];
  if (!read_self_exe(path, sizeof(path))) {
    return nullptr;
  }
  char *slash = std::strrchr(path, '/');
  if (slash != nullptr) {
    *slash = '\0';
  }
  return duplicate_string(path);
}

long long oic_now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

void oic_sleep_ms(unsigned ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#endif // !_WIN32
