/*
 * File: tiff_test.h
 * Brief: Tiff test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/tiff_test.h - CLI 测试入口（供 lead 集成 CLI；C 链接）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 打印 TIFF 文件头信息（字节序/magic/IFD/核心 tag）。返回 0 成功。
int image_codec_tiff_info(const char* path);

// 解码 TIFF -> 24-bit BMP。backend: 0=opencl(gpu), 2=cpu（解码当前统一走 CPU）。
// 返回 0 成功。
int image_codec_tiff_decode(const char* in, const char* out_bmp, int backend);

// BMP -> TIFF 编码。backend: 0=opencl(gpu), 2=cpu；
// compression: 1=none, 5=LZW；rows_per_strip: 每 strip 行数（LZW 时自动调整）。
// 返回 0 成功。
int image_codec_tiff_encode(const char* in_bmp, const char* out, int backend,
                    int compression, int rows_per_strip);

#ifdef __cplusplus
}  // extern "C"
#endif
