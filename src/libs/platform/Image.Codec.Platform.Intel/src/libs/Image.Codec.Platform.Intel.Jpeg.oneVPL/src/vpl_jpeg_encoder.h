/*
 * File: vpl_jpeg_encoder.h
 * Brief: Vpl jpeg encoder.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// vpl_jpeg_encoder.h - oneVPL 硬件 JPEG 编码器（系统内存 NV12 输入）。
//
// 与旧实现的区别：不再依赖 D3D11 / DXGI。输入帧走 MFX_IOPATTERN_IN_SYSTEM_MEMORY，
// 由 MFXMemory_GetSurfaceForEncode 取内部系统内存 surface，再把 NV12 拷进去。
// 这样 Linux（VA-API）与 Windows（D3D11，但只用系统内存）共用同一条代码路径。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vpl_device.h"

namespace oic {
namespace jpeg {
namespace onevpl {

// oneVPL 硬件 JPEG 编码器。一次 init 对应一组固定的宽高与质量，
// 同一实例可以连续编码多帧（TIFF 的每条 strip 就是一帧）。
struct VplJpegEncoder {
  VplSession vpl;
  mfxVideoParam param{};
  mfxU32 width = 0;
  mfxU32 height = 0;
  mfxU32 surface_width = 0;  // 运行库要求的输入对齐宽度
  mfxU32 surface_height = 0; // 运行库要求的输入对齐高度
  std::string impl_name;
  std::string last_error;

  VplJpegEncoder() = default;
  VplJpegEncoder(const VplJpegEncoder &) = delete;
  VplJpegEncoder &operator=(const VplJpegEncoder &) = delete;
  ~VplJpegEncoder() { close(); }

  // 建立会话并初始化编码器。quality 0..100。
  // width/height 是**真实**尺寸，可以奇数：内部把 NV12 表面宽高向上取偶、
  // 高度至少补到 16 行（Intel JPEG 编码器的最小 MCU 块），CropW/CropH 与
  // SOF 仍是真实尺寸，因此 TIFF 的奇数尺寸/短末条 strip 也能正确往返。
  bool init(uint32_t w, uint32_t h, uint32_t quality);
  // 按指定硬件单元初始化（多显卡机器上选核显时使用）。
  bool init_on_unit(const VplUnitInfo &unit, uint32_t w, uint32_t h,
                    uint32_t quality);

  // 编码一帧 NV12（w*h + w*ceil(h/2) 字节，Y 平面后接交错 UV，pitch=width）
  // -> JPEG 码流。高度为奇数时色度按 ceil(h/2) 行，不足的部分按最后一行复制。
  bool encode(const uint8_t *nv12, size_t nv12_bytes,
              std::vector<uint8_t> &jpeg);

  // RGB24(r,g,b) / BGR24(b,g,r) 便捷入口：内部转 NV12 后编码。
  bool encode_rgb24(const uint8_t *rgb, size_t rgb_bytes,
                    std::vector<uint8_t> &jpeg);
  bool encode_bgr24(const uint8_t *bgr, size_t bgr_bytes,
                    std::vector<uint8_t> &jpeg);

  // 释放会话与运行库。
  void close();

private:
  bool apply_params(uint32_t w, uint32_t h, uint32_t quality);
  // 一帧编码 + 冲刷；成功返回整段 JPEG。
  bool encode_surface(mfxFrameSurface1 *surface, std::vector<uint8_t> &jpeg);
};

} // namespace onevpl
} // namespace jpeg
} // namespace oic
