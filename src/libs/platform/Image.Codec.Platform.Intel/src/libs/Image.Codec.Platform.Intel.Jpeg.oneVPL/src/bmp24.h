/*
 * File: bmp24.h
 * Brief: Bmp24.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// bmp24.h - 24-bit 未压缩 BMP 读写（CLI 与自测程序的中转格式）。
//
// BMP 只是调试中转：真正的输入输出是 JPEG / TIFF。这里刻意只支持
// 24-bit BI_RGB，遇到别的格式明确报错，而不是猜一个能跑通的解释。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oic {
namespace jpeg {
namespace onevpl {

// 读 24-bit 未压缩 BMP -> 顶向下 BGR24（紧致 w*h*3）。
bool bmp_load_24(const char *path, uint32_t &width, uint32_t &height,
                 std::vector<uint8_t> &bgr, std::string &error);

// 写 24-bit 未压缩 BMP（bottom-up）。
bool bmp_save_24(const char *path, const uint8_t *bgr, uint32_t width,
                 uint32_t height, std::string &error);

} // namespace onevpl
} // namespace jpeg
} // namespace oic
