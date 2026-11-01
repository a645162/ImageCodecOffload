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

} // namespace ico_cli
