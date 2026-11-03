/*
 * File: vpl_jpeg_decoder.cpp
 * Brief: Vpl jpeg decoder.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// vpl_jpeg_decoder.cpp - oneVPL 硬件 JPEG 解码器实现（系统内存，无 D3D11 依赖）。
#include "vpl_jpeg_decoder.h"

#include <cstring>

#include "rgb_nv12.h"

namespace oic {
namespace jpeg {
namespace onevpl {

namespace {

constexpr mfxU32 kSyncTimeoutMs = 60000;

// 从输出 surface 拷出紧凑 NV12。返回 false 表示表面数据不可用。
bool copy_surface_to_nv12(mfxFrameSurface1 *surface, uint32_t w, uint32_t h,
                          std::vector<uint8_t> &nv12, std::string &error) {
  if (surface == nullptr || surface->Data.Y == nullptr) {
    error = "解码器返回的表面没有系统内存数据";
    return false;
  }
  const mfxU16 pitch = surface->Data.Pitch;
  if (pitch < w) {
    error = "解码表面 pitch 小于图像宽度";
    return false;
  }
  const mfxU16 crop_x = surface->Info.CropX;
  const mfxU16 crop_y = surface->Info.CropY;
  // NV12 总字节数 = Y(w*h) + UV(w*ceil(h/2))：奇数高度时色度行数与
  // 亮度行数不是整数倍关系，用 h*3/2 会少算一行。
  const uint32_t chroma_h = (h + 1) / 2;
  nv12.assign(static_cast<size_t>(w) * h + static_cast<size_t>(w) * chroma_h,
              128);
  uint8_t *dst_y = nv12.data();
  for (uint32_t row = 0; row < h; ++row) {
    const uint8_t *src =
        surface->Data.Y + static_cast<size_t>(crop_y + row) * pitch + crop_x;
    std::memcpy(dst_y + static_cast<size_t>(row) * w, src, w);
  }

  uint8_t *dst_uv = nv12.data() + static_cast<size_t>(w) * h;
  if (surface->Info.ChromaFormat == MFX_CHROMAFORMAT_YUV400 ||
      surface->Data.UV == nullptr) {
    // 灰度 JPEG：硬件给的是 YUV400，没有色度平面。补中性色度，
    // 让上层的 NV12 约定仍然成立（而不是让调用方去猜 FourCC）。
    std::memset(dst_uv, 128, static_cast<size_t>(w) * chroma_h);
    return true;
  }
  const uint32_t chroma_x = crop_x & ~1u;
  for (uint32_t row = 0; row < chroma_h; ++row) {
    const uint8_t *src = surface->Data.UV +
                         static_cast<size_t>(crop_y / 2 + row) * pitch +
                         chroma_x;
    std::memcpy(dst_uv + static_cast<size_t>(row) * w, src, w);
  }
  return true;
}

} // namespace

bool VplJpegDecoder::init() {
  std::string error;
  if (!vpl.open_intel(false, true, &error)) {
    last_error = error;
    return false;
  }
  impl_name = vpl.unit.impl_name;
  return true;
}

bool VplJpegDecoder::init_on_unit(const VplUnitInfo &unit) {
  std::string error;
  if (!vpl.open(unit, &error)) {
    last_error = error;
    return false;
  }
  impl_name = vpl.unit.impl_name;
  return true;
}

bool VplJpegDecoder::init_with_probe() {
  const VplProbeResult probe = vpl_probe();
  if (!probe.has_intel_jpeg_decoder()) {
    last_error = "未找到支持 JPEG 解码的 Intel 硬件单元";
    return false;
  }
  for (const auto &unit : probe.units) {
    if (unit.is_intel() && unit.hardware && unit.jpeg_decode) {
      return init_on_unit(unit);
    }
  }
  last_error = "未找到支持 JPEG 解码的 Intel 硬件单元";
  return false;
}

bool VplJpegDecoder::decode_nv12(const uint8_t *jpeg, size_t size,
                                 std::vector<uint8_t> &nv12, uint32_t &w,
                                 uint32_t &h) {
  nv12.clear();
  w = 0;
  h = 0;
  if (vpl.session == nullptr) {
    last_error = "解码器未初始化";
    return false;
  }
  if (jpeg == nullptr || size == 0) {
    last_error = "JPEG 输入为空";
    return false;
  }

  // 一个会话可以连续解码多张图，但 DecodeHeader/Init 每张图都要重做
  // （尺寸、采样因子都可能不同），所以先把上一个解码器关掉。
  vpl.dyn.MFXVideoDECODE_Close(vpl.session);

  mfxBitstream bitstream{};
  bitstream.Data = const_cast<mfxU8 *>(jpeg);
  bitstream.DataLength = static_cast<mfxU32>(size);
  bitstream.MaxLength = static_cast<mfxU32>(size);
  bitstream.DataOffset = 0;

  std::memset(&param, 0, sizeof(param));
  param.mfx.CodecId = MFX_CODEC_JPEG;
  mfxStatus sts =
      vpl.dyn.MFXVideoDECODE_DecodeHeader(vpl.session, &bitstream, &param);
  if (sts != MFX_ERR_NONE) {
    last_error =
        std::string("MFXVideoDECODE_DecodeHeader 失败：") + vpl_status_str(sts);
    return false;
  }

  mfxFrameInfo &info = param.mfx.FrameInfo;
  w = info.CropW != 0 ? info.CropW : info.Width;
  h = info.CropH != 0 ? info.CropH : info.Height;
  if (w == 0 || h == 0) {
    last_error = "JPEG 头里的尺寸为 0";
    return false;
  }
  info.CropX = 0;
  info.CropY = 0;
  info.CropW = static_cast<mfxU16>(w);
  info.CropH = static_cast<mfxU16>(h);
  info.FourCC = MFX_FOURCC_NV12;
  info.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
  param.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

  sts = vpl.dyn.MFXVideoDECODE_Init(vpl.session, &param);
  if (sts != MFX_ERR_NONE && sts != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
    last_error =
        std::string("MFXVideoDECODE_Init 失败：") + vpl_status_str(sts);
    return false;
  }

  bitstream.DataOffset = 0;
  bitstream.DataLength = static_cast<mfxU32>(size);
  for (int attempt = 0; attempt < 8; ++attempt) {
    mfxFrameSurface1 *surface = nullptr;
    mfxSyncPoint sync = nullptr;
    const mfxStatus dec_sts = vpl.dyn.MFXVideoDECODE_DecodeFrameAsync(
        vpl.session, &bitstream, nullptr, &surface, &sync);
    if (dec_sts == MFX_ERR_MORE_DATA) {
      break; // 单张 JPEG 已无更多输出
    }
    if (dec_sts != MFX_ERR_NONE && dec_sts != MFX_WRN_IN_EXECUTION) {
      last_error = std::string("MFXVideoDECODE_DecodeFrameAsync 失败：") +
                   vpl_status_str(dec_sts);
      return false;
    }
    if (sync == nullptr) {
      continue; // 需要更多输入才能出帧
    }
    const mfxStatus sync_sts =
        vpl.dyn.MFXVideoCORE_SyncOperation(vpl.session, sync, kSyncTimeoutMs);
    if (sync_sts != MFX_ERR_NONE) {
      if (surface != nullptr && surface->FrameInterface != nullptr) {
        surface->FrameInterface->Release(surface);
      }
      last_error = std::string("MFXVideoCORE_SyncOperation 失败：") +
                   vpl_status_str(sync_sts);
      return false;
    }
    if (surface == nullptr) {
      continue;
    }
    // 系统内存输出 surface 同样要先 Map 才能读 Data.Y/UV（dispatcher 不提供
    // MFXMemory_MapFrame）。读出来的指针只在 Unmap 之前有效。
    if (surface->FrameInterface == nullptr ||
        surface->FrameInterface->Map == nullptr ||
        surface->FrameInterface->Unmap == nullptr) {
      surface->FrameInterface->Release(surface);
      last_error = "运行库返回的 surface 没有 Map/Unmap 接口";
      return false;
    }
    const mfxStatus map_sts =
        surface->FrameInterface->Map(surface, MFX_MAP_READ);
    if (map_sts != MFX_ERR_NONE) {
      surface->FrameInterface->Release(surface);
      last_error = std::string("surface Map 失败：") + vpl_status_str(map_sts);
      return false;
    }
    const bool ok = copy_surface_to_nv12(surface, w, h, nv12, last_error);
    surface->FrameInterface->Unmap(surface);
    if (surface->FrameInterface != nullptr) {
      surface->FrameInterface->Release(surface);
    }
    return ok;
  }

  if (last_error.empty()) {
    last_error = "硬件解码器没有输出帧";
  }
  return false;
}

bool VplJpegDecoder::decode_bgr24(const uint8_t *jpeg, size_t size,
                                  std::vector<uint8_t> &bgr, uint32_t &w,
                                  uint32_t &h) {
  std::vector<uint8_t> nv12;
  if (!decode_nv12(jpeg, size, nv12, w, h)) {
    return false;
  }
  bgr.assign(static_cast<size_t>(w) * h * 3, 0);
  nv12_to_bgr24(nv12.data(), bgr.data(), w, h);
  return true;
}

bool VplJpegDecoder::decode(const uint8_t *jpeg, size_t size,
                            std::vector<uint8_t> &bgra, uint32_t &w,
                            uint32_t &h) {
  std::vector<uint8_t> nv12;
  if (!decode_nv12(jpeg, size, nv12, w, h)) {
    return false;
  }
  bgra.assign(static_cast<size_t>(w) * h * 4, 255);
  nv12_to_bgra32(nv12.data(), bgra.data(), w, h);
  return true;
}

void VplJpegDecoder::close() {
  if (vpl.session != nullptr) {
    vpl.dyn.MFXVideoDECODE_Close(vpl.session);
  }
  vpl.close();
}

} // namespace onevpl
} // namespace jpeg
} // namespace oic
