/*
 * File: cli_common.h
 * Brief: CLI shared helpers.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-11-01
 */

// cli_common.h - ico 各子命令共用的文件读写、图像装载与时间工具。
//
// 为什么要单独一份：矩阵命令（matrix / matrix-metrics）需要与 encode/decode
// 完全相同的「BMP 走中转、其它走路由层解码」语义。任何一边各写一份读图逻辑，
// 迟早会出现「同一张图在两条命令里被解码成不同通道数」这种无法复现的偏差。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <atomic>
#include <thread>

#include "backend.h"
#include "image_codec_types.h"
#include "router.h"

namespace ico_cli {

// CSV 字段转义：只有含分隔符/引号/换行时才加引号，避免报告里出现无谓的引号。
std::string csvField(const std::string &value);

bool readFile(const std::string &path, std::vector<uint8_t> *out,
              std::string *err);
// 只读文件开头若干字节：用于「先看魔数/BMP 头再决定要不要整套读进来」。
bool readPrefix(const std::string &path, size_t max_bytes,
                std::vector<uint8_t> *out, std::string *err);
bool writeFile(const std::string &path, const std::vector<uint8_t> &data,
               std::string *err);

bool hasExtension(const std::string &path, const char *ext);
bool isBmp(const std::vector<uint8_t> &data);
image_codec::offload::Format
formatFromPathOrBytes(const std::string &path,
                      const std::vector<uint8_t> &data);
bool parseBackend(const std::string &value,
                  image_codec::offload::BackendKind *out);

// 读一张图：BMP 走中转格式，其它走路由层解码。
bool loadImage(image_codec::offload::Router &router, const std::string &path,
               image_codec::offload::Image *out, std::string *err);

long long nowMicros();

// 进程当前的常驻主存（RSS），单位字节。读不到返回 0——报告里的 0 表示
// 「没测到」，绝不表示「没占内存」。
uint64_t processResidentBytes();

// 一次矩阵格子测量期间的主存高水位采样器。
//
// 为什么需要在后台线程里反复采样：编码一张 1.9 GB 的帧，峰值出现在**过程
// 中间**（源缓冲 + 中转 + 编码输出同时在手），格子结束后再看一眼 RSS 只会
// 看到已经回落或还没涨上去的数字。采样失败时 stop() 返回 0，调用方据此
// 写空字段，而不是写一个像样的假数字。
class HostPeakSampler final {
public:
  HostPeakSampler() = default;
  ~HostPeakSampler();
  HostPeakSampler(const HostPeakSampler &) = delete;
  HostPeakSampler &operator=(const HostPeakSampler &) = delete;

  // 开始采样前先记一次当前 RSS，保证极短的格子也有一个非零下界。
  void start();
  // 停止采样并返回这期间观察到的 RSS 峰值（字节）；从未采到值则返回 0。
  uint64_t stop();

private:
  void loop();

  std::atomic<bool> running_{false};
  std::atomic<uint64_t> peak_{0};
  std::thread thread_;
};

} // namespace ico_cli
