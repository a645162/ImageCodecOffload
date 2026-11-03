/*
 * File: vpl_jpeg_decoder.h
 * Brief: Vpl jpeg decoder.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// vpl_jpeg_decoder.h - oneVPL 硬件 JPEG 解码器（系统内存 NV12 输出）。
//
// 与旧实现的区别：不再创建 D3D11 设备，也不自带帧分配器。解码输出走
// MFX_IOPATTERN_OUT_SYSTEM_MEMORY + 运行库内部 surface（DecodeFrameAsync 传
// nullptr 工作表面），由解码器自己分配；取到 surface 后按 pitch 拷出 NV12，
// 再转成 BGR24 / BGRA32。Linux（VA-API）与 Windows 共用这条路径。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vpl_device.h"

namespace oic {
namespace jpeg {
namespace onevpl {

struct VplJpegDecoder {
  VplSession vpl;
  mfxVideoParam param{};
  std::string impl_name;
  std::string last_error;

  VplJpegDecoder() = default;
  VplJpegDecoder(const VplJpegDecoder &) = delete;
  VplJpegDecoder &operator=(const VplJpegDecoder &) = delete;
  ~VplJpegDecoder() { close(); }

  // 建立会话（按住所选 Intel 硬件单元）。失败返回 false 并写 last_error。
  bool init();
  bool init_on_unit(const VplUnitInfo &unit);
  // 探测并建立会话，把探测结果留在 last_probe 便于上层打印原因。
  bool init_with_probe();

  // 解码一帧 JPEG -> BGRA32（w*h*4，alpha=255）。成功时写回 w/h。
  bool decode(const uint8_t *jpeg, size_t size, std::vector<uint8_t> &bgra,
              uint32_t &w, uint32_t &h);
  // 解码一帧 JPEG -> BGR24（w*h*3）。
  bool decode_bgr24(const uint8_t *jpeg, size_t size, std::vector<uint8_t> &bgr,
                    uint32_t &w, uint32_t &h);
  // 解码一帧 JPEG -> NV12（w*h*3/2，pitch=width）。供需要原始 YUV 的调用方。
  bool decode_nv12(const uint8_t *jpeg, size_t size, std::vector<uint8_t> &nv12,
                   uint32_t &w, uint32_t &h);

  void close();
};

} // namespace onevpl
} // namespace jpeg
} // namespace oic
