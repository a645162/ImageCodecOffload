/*
 * File: win_platform.cpp
 * Brief: Win platform.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#include "image_codec_platform.h"

#if defined(_WIN32)
#include <windows.h>

#include <cstdlib>
#include <cstring>

char *image_codec_exe_dir(void) {
  char buf[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return nullptr;
  }
  char *slash = std::strrchr(buf, '\\');
  if (slash != nullptr) {
    *slash = '\0';
  }
  const size_t len = std::strlen(buf);
  char *out = static_cast<char *>(std::malloc(len + 1));
  if (out == nullptr) {
    return nullptr;
  }
  std::memcpy(out, buf, len + 1);
  return out;
}

long long image_codec_now_ns(void) {
  LARGE_INTEGER freq, counter;
  if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
    return 0;
  }
  QueryPerformanceCounter(&counter);
  return static_cast<long long>(
      (static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart)) *
      1e9);
}

void image_codec_sleep_ms(unsigned ms) { Sleep(ms); }

#else
// POSIX 预留实现（阶段0 仅 Windows 主路径；Linux/macOS 后续补充）。
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <thread>

char *image_codec_exe_dir(void) { return nullptr; }

long long image_codec_now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

void image_codec_sleep_ms(unsigned ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
#endif
