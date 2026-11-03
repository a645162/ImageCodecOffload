/*
 * File: image_codec_platform.h
 * Brief: Image codec platform.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// platform/image_codec_platform.h - 内部平台抽象（不对外导出）。
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 可执行文件所在目录（malloc'd，调用方 free；失败返回 NULL）。
char *image_codec_exe_dir(void);

// 单调时钟纳秒（用于计时基准）。
long long image_codec_now_ns(void);

// 忙等毫秒。
void image_codec_sleep_ms(unsigned ms);

#ifdef __cplusplus
}
#endif
