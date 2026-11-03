/*
 * File: png_test.h
 * Brief: Png test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// png_test.h - PNG 模块的文件入口（供上层 C API 集成，签名固定，C 链接）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 打印 PNG 文件信息：尺寸/位深/color type/通道数。成功返回 0。
int image_codec_png_info(const char *path);

// 解码 PNG -> BMP（1 通道 -> 8-bit 灰度 BMP，3/4 通道 -> 24-bit RGB BMP）。
// backend: 0=OpenCL（行滤波），2=CPU。成功返回 0。
int image_codec_png_decode(const char *in, const char *out_bmp, int backend);

// 编码 BMP -> PNG。compression: 0..9（<0 用默认 6）。
// filter: -1=自适应，0..4=固定滤波器。backend: 0=OpenCL，2=CPU。
// 成功返回 0。
int image_codec_png_encode(const char *in_bmp, const char *out, int backend,
                           int compression, int filter);

#ifdef __cplusplus
}
#endif
