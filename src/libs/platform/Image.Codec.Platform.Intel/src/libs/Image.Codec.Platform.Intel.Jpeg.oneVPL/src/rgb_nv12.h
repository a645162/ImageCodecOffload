/*
 * File: rgb_nv12.h
 * Brief: Rgb nv12.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// rgb_nv12.h - 24-bit RGB/BGR <-> NV12 转换（CPU 实现，BT.601 全范围 / JPEG）。
//
// NV12 布局（与 oneVPL 表面一致）：
//   总字节数 w*h*3/2，Y 平面在前（w*h 字节，pitch=w），
//   之后是交错 UV 平面（w*(h/2) 字节，pitch=w，U 在前 V 在后）。
// 4:2:0 子采样要求 w、h 为偶数；若为奇数则按向下取偶覆盖，
// 转换函数本身不做边界外写入。
#pragma once

#include <cstdint>

namespace oic {
namespace jpeg {
namespace onevpl {

// BGR24 (b,g,r) -> NV12。
void bgr24_to_nv12(const uint8_t* bgr, uint8_t* nv12, uint32_t w, uint32_t h);

// RGB24 (r,g,b) -> NV12。
void rgb24_to_nv12(const uint8_t* rgb, uint8_t* nv12, uint32_t w, uint32_t h);

// NV12 -> BGR24（紧致 w*h*3）。
void nv12_to_bgr24(const uint8_t* nv12, uint8_t* bgr, uint32_t w, uint32_t h);

// NV12 -> RGB24（紧致 w*h*3）。
void nv12_to_rgb24(const uint8_t* nv12, uint8_t* rgb, uint32_t w, uint32_t h);

// NV12 -> BGRA32（alpha=255），供解码器直接输出到内存。
void nv12_to_bgra32(const uint8_t* nv12, uint8_t* bgra, uint32_t w, uint32_t h);

}  // namespace onevpl
}  // namespace jpeg
}  // namespace oic
