/*
 * File: bmp_io.h
 * Brief: Bmp io.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// bmp_io.h - BMP 读写（8-bit 灰度 / 24-bit RGB / 32-bit RGBA），top-down RGB 语义。
//
// 位置说明：BMP 在本库里是**模块之间的中转格式**（CLI 与文件级便捷入口用它，
// C API 的 buffer 路径已经不走磁盘），不是产品路径。放在 Common 里是为了让
// TIFF / PNG / JPEG 三个模块共用同一份实现，而不是各自复制一份只在细节上不同的版本。
//
// 通道保真：写 8-bit 灰度就是 1 通道，写 32-bit 就是 4 通道（Alpha 原样保留），
// 绝不为了省事升成 24-bit RGB；读的时候也按 bpp 决定 channels。
// **2 通道（灰度+Alpha）没有 BMP 表示法**：BMP 的位深只有 1/4/8/24/32，
// 所以 bmp_write 对 channels==2 明确返回 -2，而不是伪造一个 32-bit 文件。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"

namespace image_codec {

// 读取 BMP（24-bit BI_RGB、32-bit BI_RGB/BI_BITFIELDS、8-bit 灰度 + 调色板）。
//   8-bit 时按调色板索引取样本（本库自己写出的灰度 BMP 使用恒等灰度调色板，
//   因此索引即灰度值；外部任意调色板也按索引解释，不做颜色空间猜测）。
//   32-bit BI_RGB 的第四字节按规范是保留位，因此 Alpha 一律补 255（不透明）；
//   只有 BI_BITFIELDS 明确给出 A 掩码时才采用文件里的 Alpha。
// 成功返回 0；不支持的格式返回 -2；I/O 失败返回 -1。
int bmp_read(const std::string &path, ImageBuffer *img, std::string *err);

// 按 img.channels 写出 BMP：1 -> 8-bit 灰度（恒等调色板）、3 -> 24-bit RGB、
// 4 -> 32-bit BI_BITFIELDS（RGBA 掩码，Alpha 原样写出）。channels==2 返回 -2。
// 成功返回 0。
int bmp_write(const std::string &path, const ImageBuffer &img,
              std::string *err);

// 便捷：从内存中的 top-down 像素写出 BMP（不复制到 ImageBuffer）。
int bmp_write_pixels(const std::string &path, const uint8_t *pixels, int width,
                     int height, int channels, std::string *err);

} // namespace image_codec
