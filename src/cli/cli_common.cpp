/*
 * File: cli_common.cpp
 * Brief: CLI shared helpers.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-11-01
 */

#include "cli_common.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <atomic>
#include <thread>

#include "bmp_io.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <fstream>
#include <unistd.h>
#endif

namespace ico_cli {

std::string csvField(const std::string &value) {
  if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const char ch : value) {
    if (ch == '"') escaped.push_back('"');
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

bool readFile(const std::string &path, std::vector<uint8_t> *out,
              std::string *err) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    if (err != nullptr) *err = "cannot open " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(f);
    if (err != nullptr) *err = "cannot stat " + path;
    return false;
  }
  out->resize(static_cast<size_t>(size));
  const size_t read =
      out->empty() ? 0 : std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  if (read != out->size()) {
    if (err != nullptr) *err = "short read on " + path;
    return false;
  }
  return true;
}

bool readPrefix(const std::string &path, size_t max_bytes,
                std::vector<uint8_t> *out, std::string *err) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    if (err != nullptr) *err = "cannot open " + path;
    return false;
  }
  out->resize(max_bytes);
  const size_t read = std::fread(out->data(), 1, max_bytes, f);
  std::fclose(f);
  out->resize(read);
  return true;
}

bool writeFile(const std::string &path, const std::vector<uint8_t> &data,
               std::string *err) {
  std::FILE *f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    if (err != nullptr) *err = "cannot create " + path;
    return false;
  }
  const size_t written =
      data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
  if (written != data.size()) {
    if (err != nullptr) *err = "short write on " + path;
    return false;
  }
  return true;
}

bool hasExtension(const std::string &path, const char *ext) {
  if (path.size() < std::strlen(ext)) return false;
  std::string tail = path.substr(path.size() - std::strlen(ext));
  for (char &c : tail) c = static_cast<char>(std::tolower(c));
  return tail == ext;
}

bool isBmp(const std::vector<uint8_t> &data) {
  return data.size() >= 2 && data[0] == 'B' && data[1] == 'M';
}

image_codec::offload::Format
formatFromPathOrBytes(const std::string &path,
                      const std::vector<uint8_t> &data) {
  const auto probed =
      image_codec::offload::probeFormat(data.data(), data.size());
  if (probed != image_codec::offload::Format::Unknown) return probed;

  const std::size_t dot = path.find_last_of('.');
  if (dot != std::string::npos) {
    return image_codec::offload::formatFromExtension(path.substr(dot));
  }
  return image_codec::offload::Format::Unknown;
}

bool parseBackend(const std::string &value,
                  image_codec::offload::BackendKind *out) {
  if (value == "cpu") {
    *out = image_codec::offload::BackendKind::Cpu;
    return true;
  }
  if (value == "opencl") {
    *out = image_codec::offload::BackendKind::OpenCl;
    return true;
  }
  if (value == "cuda") {
    *out = image_codec::offload::BackendKind::Cuda;
    return true;
  }
  if (value == "vulkan") {
    *out = image_codec::offload::BackendKind::Vulkan;
    return true;
  }
  return false;
}

// 读一张图：BMP 走中转格式，其它走路由层解码。
bool loadImage(image_codec::offload::Router &router, const std::string &path,
               image_codec::offload::Image *out, std::string *err) {
  std::vector<uint8_t> bytes;
  if (!readFile(path, &bytes, err)) return false;

  if (isBmp(bytes)) {
    auto bmp =
        image_codec::offload::common::readBmp(bytes.data(), bytes.size());
    if (!bmp) {
      if (err != nullptr) *err = "bmp: " + bmp.error().message;
      return false;
    }
    *out = std::move(*bmp);
    return true;
  }

  const auto format = formatFromPathOrBytes(path, bytes);
  if (format == image_codec::offload::Format::Unknown) {
    if (err != nullptr) *err = "unknown container format: " + path;
    return false;
  }

  const auto decoded = router.decode(format, bytes.data(), bytes.size());
  if (!decoded) {
    if (err != nullptr) *err = decoded.error().message;
    return false;
  }
  *out = std::move(*decoded);
  return true;
}

long long nowMicros() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::microseconds>(
             clock::now().time_since_epoch())
      .count();
}

uint64_t processResidentBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(
                               &counters),
                           sizeof(counters)) == 0) {
    return 0;
  }
  return static_cast<uint64_t>(counters.WorkingSetSize);
#else
  // 用 /proc/self/statm 而不是 status：一行七个数，第二列就是 RSS 页数，
  // 不需要解析 "VmRSS:" 这种会随内核版本变化的标签。
  std::ifstream statm("/proc/self/statm");
  if (!statm) return 0;
  uint64_t total_pages = 0;
  uint64_t resident_pages = 0;
  if (!(statm >> total_pages >> resident_pages)) return 0;
  (void)total_pages;
  return resident_pages * static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

HostPeakSampler::~HostPeakSampler() { stop(); }

void HostPeakSampler::start() {
  if (running_.exchange(true)) return;
  const uint64_t initial = processResidentBytes();
  uint64_t expected = 0;
  peak_.compare_exchange_strong(expected, initial);
  thread_ = std::thread([this]() { loop(); });
}

uint64_t HostPeakSampler::stop() {
  if (!running_.exchange(false)) return peak_.load();
  if (thread_.joinable()) thread_.join();
  return peak_.load();
}

void HostPeakSampler::loop() {
  // 10 ms 一次：1.9 GB 的帧编码是秒级，这段粒度足以抓到峰值；比 1 ms 更密
  // 只是给 CPU 添噪声，不会让峰值更准。
  while (running_.load(std::memory_order_relaxed)) {
    const uint64_t rss = processResidentBytes();
    if (rss != 0) {
      uint64_t current = peak_.load(std::memory_order_relaxed);
      while (rss > current &&
             !peak_.compare_exchange_weak(current, rss,
                                          std::memory_order_relaxed)) {
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

} // namespace ico_cli
