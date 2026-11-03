/*
 * File: win_platform.cpp
 * Brief: Win platform.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// win_platform.cpp - Windows 平台抽象实现（可执行目录 / 单调时钟 / 休眠）。
#include "oic_platform.h"

#if defined(_WIN32)
#include <windows.h>

#include <cstdlib>
#include <cstring>

char *oic_exe_dir(void) {
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

long long oic_now_ns(void) {
  LARGE_INTEGER freq, counter;
  if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
    return 0;
  }
  QueryPerformanceCounter(&counter);
  return static_cast<long long>((static_cast<double>(counter.QuadPart) /
                                 static_cast<double>(freq.QuadPart)) *
                                1e9);
}

void oic_sleep_ms(unsigned ms) { Sleep(ms); }

#endif // _WIN32
