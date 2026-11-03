/*
 * File: onevpl_test.h
 * Brief: Onevpl test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// onevpl_test.h - oneVPL JPEG 后端的 C 入口（供 CLI / 集成方调用，签名固定）。
//
// 所有函数返回 0 表示成功，非 0 表示失败（错误已打印到 stderr）。
// 没有运行库 / 没有 Intel 硬件单元 / 没有媒体引擎时返回明确错误码，不崩溃。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 编码：in_bmp(24-bit 未压缩 BMP) -> out_jpeg(JPEG)。quality 0..100。
int oic_onevpl_encode(const char *in_bmp, const char *out_jpeg, int quality);

// 解码：in_jpeg -> out_bmp(24-bit 未压缩 BMP)。
int oic_onevpl_decode(const char *in_jpeg, const char *out_bmp);

// 探测：0 = 运行库可用（可加载且全部 MFX 入口可解析），1 = 不可用。
int oic_onevpl_available(void);

// 硬件单元报告（UTF-8，多行）。返回写入的字节数（不含结尾 0），
// capacity 不足时截断并返回所需长度；capacity<=0 时只返回所需长度。
int oic_onevpl_probe(char *out, int capacity);

#ifdef __cplusplus
}
#endif
