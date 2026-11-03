/*
 * File: vpl_jpeg_encoder.cpp
 * Brief: Vpl jpeg encoder.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// vpl_jpeg_encoder.cpp - oneVPL 硬件 JPEG 编码器实现（系统内存，无 D3D11 依赖）。
#include "vpl_jpeg_encoder.h"

#include <algorithm>
#include <cstring>

#include "rgb_nv12.h"

namespace oic {
namespace jpeg {
namespace onevpl {

namespace {

constexpr mfxU32 kSyncTimeoutMs = 60000;
constexpr size_t kInitialBitstreamBytes = 256 * 1024;
constexpr mfxU16 kMinJpegQuality = 1;
constexpr mfxU16 kMaxJpegQuality = 100;

// 把运行库给的对齐尺寸算成实际要拷贝的边界：既不能超过 surface，
// 也不能把调用方的数据读越界。
mfxU16 clamp_u16(uint32_t value) {
  return value > 0xFFFF ? 0xFFFF : static_cast<mfxU16>(value);
}

} // namespace

bool VplJpegEncoder::init(uint32_t w, uint32_t h, uint32_t quality) {
  std::string error;
  if (!vpl.open_intel(true, false, &error)) {
    last_error = error;
    return false;
  }
  impl_name = vpl.unit.impl_name;
  if (!apply_params(w, h, quality)) {
    close();
    return false;
  }
  return true;
}

bool VplJpegEncoder::init_on_unit(const VplUnitInfo &unit, uint32_t w,
                                  uint32_t h, uint32_t quality) {
  std::string error;
  if (!vpl.open(unit, &error)) {
    last_error = error;
    return false;
  }
  impl_name = vpl.unit.impl_name;
  if (!apply_params(w, h, quality)) {
    close();
    return false;
  }
  return true;
}

bool VplJpegEncoder::apply_params(uint32_t w, uint32_t h, uint32_t quality) {
  width = w;
  height = h;
  // 尺寸不要求偶数：oneVPL 的 JPEG 能力表 Width/Height 步长都是 1，
  // 运行库内部会按 MCU 补边，SOF 里仍写真实宽高。TIFF 侧也依赖这一点
  // （libtiff 会拿 SOF 宽高与 ImageWidth/ImageLength 比对，补过边的尺寸
  // 会被判成「strip 比图像还大」而拒绝解码）。
  if (w == 0 || h == 0) {
    last_error = "尺寸不能为 0";
    return false;
  }

  std::memset(&param, 0, sizeof(param));
  param.mfx.CodecId = MFX_CODEC_JPEG;
  param.mfx.CodecProfile = MFX_PROFILE_JPEG_BASELINE;
  param.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
  param.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
  // 尺寸不要求偶数：NV12 只是**表面**宽高必须是偶数（2x2 色度子采样），
  // 所以 Width/Height 向上取偶、CropW/CropH 保留真实宽高。运行库按
  // CropW/CropH 写 SOF，于是 TIFF 的末条 strip（可能是奇数行/奇数宽）
  // 也能得到真实尺寸的 JPEG，不会出现「SOF 比 strip 大」这种 libtiff
  // 会拒绝解码的形态（宽度方向上 libtiff 是硬错误，不是警告）。
  //
  // 表面高度还有下限：Intel 的 JPEG 编码器按 16 行 MCU 工作，Height < 16
  // 时 Init 直接返回 MFX_ERR_INVALID_VIDEO_PARAM（实测 257x8 失败、
  // 257x16 成功）。补到 16 行不影响码流——JPEG 的 MCU 填充本来就按
  // 16 行对齐，SOF 仍写真实的 CropH。
  const uint32_t surface_w = (w + 1u) & ~1u;
  const uint32_t surface_h = std::max<uint32_t>(16u, (h + 1u) & ~1u);
  param.mfx.FrameInfo.Width = clamp_u16(surface_w);
  param.mfx.FrameInfo.Height = clamp_u16(surface_h);
  param.mfx.FrameInfo.CropX = 0;
  param.mfx.FrameInfo.CropY = 0;
  param.mfx.FrameInfo.CropW = clamp_u16(w);
  param.mfx.FrameInfo.CropH = clamp_u16(h);
  param.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
  param.mfx.FrameInfo.FrameRateExtN = 30;
  param.mfx.FrameInfo.FrameRateExtD = 1;
  param.mfx.FrameInfo.BitDepthLuma = 8;
  param.mfx.FrameInfo.BitDepthChroma = 8;
  mfxU16 q = static_cast<mfxU16>(quality);
  param.mfx.Quality = q < kMinJpegQuality
                          ? kMinJpegQuality
                          : (q > kMaxJpegQuality ? kMaxJpegQuality : q);
  param.mfx.Interleaved = 1;
  param.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

  // Query 允许运行库调整参数（对齐等），最多重试三次。
  mfxStatus sts = MFX_ERR_NONE;
  for (int attempt = 0; attempt < 3; ++attempt) {
    sts = vpl.dyn.MFXVideoENCODE_Query(vpl.session, &param, &param);
    if (sts != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
      break;
    }
  }
  if (sts != MFX_ERR_NONE) {
    last_error =
        std::string("MFXVideoENCODE_Query 失败：") + vpl_status_str(sts);
    return false;
  }

  mfxFrameAllocRequest request{};
  sts = vpl.dyn.MFXVideoENCODE_QueryIOSurf(vpl.session, &param, &request);
  if (sts != MFX_ERR_NONE && sts != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
    last_error =
        std::string("MFXVideoENCODE_QueryIOSurf 失败：") + vpl_status_str(sts);
    return false;
  }

  sts = vpl.dyn.MFXVideoENCODE_Init(vpl.session, &param);
  if (sts != MFX_ERR_NONE && sts != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
    last_error =
        std::string("MFXVideoENCODE_Init 失败：") + vpl_status_str(sts);
    return false;
  }

  surface_width = param.mfx.FrameInfo.Width;
  surface_height = param.mfx.FrameInfo.Height;
  if (surface_width < width || surface_height < height) {
    last_error = "运行库返回的对齐尺寸小于请求尺寸";
    return false;
  }
  return true;
}

bool VplJpegEncoder::encode(const uint8_t *nv12, size_t nv12_bytes,
                            std::vector<uint8_t> &jpeg) {
  jpeg.clear();
  if (vpl.session == nullptr) {
    last_error = "编码器未初始化";
    return false;
  }
  if (nv12 == nullptr) {
    last_error = "NV12 输入为空";
    return false;
  }
  const size_t chroma_h = (static_cast<size_t>(height) + 1) / 2;
  const size_t need = static_cast<size_t>(width) * height +
                      static_cast<size_t>(width) * chroma_h;
  if (nv12_bytes < need) {
    last_error = "NV12 输入长度不足";
    return false;
  }

  mfxFrameSurface1 *surface = nullptr;
  mfxStatus sts = vpl.dyn.MFXMemory_GetSurfaceForEncode(vpl.session, &surface);
  if (sts != MFX_ERR_NONE || surface == nullptr) {
    last_error = std::string("MFXMemory_GetSurfaceForEncode 失败：") +
                 vpl_status_str(sts);
    return false;
  }

  auto release_surface = [surface]() {
    if (surface->FrameInterface != nullptr) {
      surface->FrameInterface->Release(surface);
    }
  };

  // 系统内存 surface 在 Map 之前 Data.Y/UV 是空的（dispatcher 没有
  // MFXMemory_MapFrame，只能用 FrameInterface->Map）。写完后必须 Unmap：
  // 编码器要求输入 surface 的 Data.Locked == 0，没解映射会返回
  // MFX_ERR_LOCK_MEMORY，看起来却像「尺寸不足」。
  if (surface->FrameInterface == nullptr ||
      surface->FrameInterface->Map == nullptr ||
      surface->FrameInterface->Unmap == nullptr) {
    last_error = "运行库返回的 surface 没有 Map/Unmap 接口";
    release_surface();
    return false;
  }
  sts = surface->FrameInterface->Map(surface, MFX_MAP_WRITE);
  if (sts != MFX_ERR_NONE) {
    last_error = std::string("surface Map 失败：") + vpl_status_str(sts);
    release_surface();
    return false;
  }

  // 内部 surface 可能是对齐后的尺寸，按 pitch 逐行拷贝；padding 区填
  // Y=0 / UV=128，保证编码器读到的不是未初始化内存。
  const mfxU16 pitch = surface->Data.Pitch;
  const mfxU32 surface_rows = surface->Info.Height;
  if (surface->Data.Y == nullptr || surface->Data.UV == nullptr ||
      pitch < width || surface_rows < height) {
    last_error = "运行库返回的系统内存 surface 尺寸不足";
    surface->FrameInterface->Unmap(surface);
    release_surface();
    return false;
  }
  for (mfxU32 row = 0; row < surface_rows; ++row) {
    const uint32_t src_row = row < height ? row : height - 1;
    std::memset(surface->Data.Y + static_cast<size_t>(row) * pitch, 0, pitch);
    std::memcpy(surface->Data.Y + static_cast<size_t>(row) * pitch,
                nv12 + static_cast<size_t>(src_row) * width, width);
  }
  const uint8_t *src_uv = nv12 + static_cast<size_t>(width) * height;
  const mfxU32 surface_chroma_rows = (surface_rows + 1) / 2;
  for (mfxU32 row = 0; row < surface_chroma_rows; ++row) {
    const uint32_t src_row =
        row < chroma_h ? row : static_cast<uint32_t>(chroma_h - 1);
    std::memset(surface->Data.UV + static_cast<size_t>(row) * pitch, 128,
                pitch);
    std::memcpy(surface->Data.UV + static_cast<size_t>(row) * pitch,
                src_uv + static_cast<size_t>(src_row) * width, width);
  }

  const mfxStatus unmap_sts = surface->FrameInterface->Unmap(surface);
  if (unmap_sts != MFX_ERR_NONE) {
    last_error =
        std::string("surface Unmap 失败：") + vpl_status_str(unmap_sts);
    release_surface();
    return false;
  }

  const bool ok = encode_surface(surface, jpeg);
  release_surface();
  return ok;
}

bool VplJpegEncoder::encode_surface(mfxFrameSurface1 *surface,
                                    std::vector<uint8_t> &jpeg) {
  std::vector<uint8_t> buffer(kInitialBitstreamBytes);
  mfxBitstream bitstream{};
  bitstream.MaxLength = static_cast<mfxU32>(buffer.size());
  bitstream.Data = buffer.data();

  // 单帧编码：EncodeFrameAsync 可能返回 NOT_ENOUGH_BUFFER（扩缓冲重试）、
  // MORE_DATA（等下一帧，单帧场景忽略）或 NONE（同步点已给出）。
  bool produced = false;
  for (int attempt = 0; attempt < 16 && !produced; ++attempt) {
    bitstream.DataOffset = 0;
    bitstream.DataLength = 0;
    mfxSyncPoint sync = nullptr;
    const mfxStatus sts = vpl.dyn.MFXVideoENCODE_EncodeFrameAsync(
        vpl.session, nullptr, surface, &bitstream, &sync);
    if (sts == MFX_ERR_NONE) {
      if (sync != nullptr) {
        const mfxStatus sync_sts = vpl.dyn.MFXVideoCORE_SyncOperation(
            vpl.session, sync, kSyncTimeoutMs);
        if (sync_sts != MFX_ERR_NONE) {
          last_error = std::string("MFXVideoCORE_SyncOperation 失败：") +
                       vpl_status_str(sync_sts);
          return false;
        }
      }
      jpeg.insert(jpeg.end(), bitstream.Data + bitstream.DataOffset,
                  bitstream.Data + bitstream.DataOffset + bitstream.DataLength);
      produced = true;
      break;
    }
    if (sts == MFX_ERR_MORE_DATA) {
      continue;
    }
    if (sts == MFX_ERR_NOT_ENOUGH_BUFFER) {
      buffer.resize(buffer.size() * 2);
      bitstream.MaxLength = static_cast<mfxU32>(buffer.size());
      bitstream.Data = buffer.data();
      continue;
    }
    last_error = std::string("MFXVideoENCODE_EncodeFrameAsync 失败：") +
                 vpl_status_str(sts);
    return false;
  }
  if (!produced || jpeg.empty()) {
    if (last_error.empty()) {
      last_error = "硬件编码器没有输出码流";
    }
    return false;
  }

  // 冲刷：单帧编码器通常在这里就不再输出，但延迟输出是合法的。
  for (int attempt = 0; attempt < 8; ++attempt) {
    bitstream.DataOffset = 0;
    bitstream.DataLength = 0;
    mfxSyncPoint sync = nullptr;
    const mfxStatus sts = vpl.dyn.MFXVideoENCODE_EncodeFrameAsync(
        vpl.session, nullptr, nullptr, &bitstream, &sync);
    if (sts == MFX_ERR_MORE_DATA) {
      break;
    }
    if (sts == MFX_ERR_NOT_ENOUGH_BUFFER) {
      buffer.resize(buffer.size() * 2);
      bitstream.MaxLength = static_cast<mfxU32>(buffer.size());
      bitstream.Data = buffer.data();
      continue;
    }
    if (sts != MFX_ERR_NONE) {
      break;
    }
    if (sync != nullptr) {
      vpl.dyn.MFXVideoCORE_SyncOperation(vpl.session, sync, kSyncTimeoutMs);
    }
    jpeg.insert(jpeg.end(), bitstream.Data + bitstream.DataOffset,
                bitstream.Data + bitstream.DataOffset + bitstream.DataLength);
  }
  return !jpeg.empty();
}

bool VplJpegEncoder::encode_rgb24(const uint8_t *rgb, size_t rgb_bytes,
                                  std::vector<uint8_t> &jpeg) {
  const size_t need = static_cast<size_t>(width) * height * 3;
  if (rgb == nullptr || rgb_bytes < need) {
    last_error = "RGB24 输入长度不足";
    return false;
  }
  const size_t chroma_bytes = static_cast<size_t>(width) * ((height + 1) / 2);
  std::vector<uint8_t> nv12(static_cast<size_t>(width) * height + chroma_bytes,
                            128);
  rgb24_to_nv12(rgb, nv12.data(), width, height);
  return encode(nv12.data(), nv12.size(), jpeg);
}

bool VplJpegEncoder::encode_bgr24(const uint8_t *bgr, size_t bgr_bytes,
                                  std::vector<uint8_t> &jpeg) {
  const size_t need = static_cast<size_t>(width) * height * 3;
  if (bgr == nullptr || bgr_bytes < need) {
    last_error = "BGR24 输入长度不足";
    return false;
  }
  const size_t chroma_bytes = static_cast<size_t>(width) * ((height + 1) / 2);
  std::vector<uint8_t> nv12(static_cast<size_t>(width) * height + chroma_bytes,
                            128);
  bgr24_to_nv12(bgr, nv12.data(), width, height);
  return encode(nv12.data(), nv12.size(), jpeg);
}

void VplJpegEncoder::close() {
  if (vpl.session != nullptr) {
    vpl.dyn.MFXVideoENCODE_Close(vpl.session);
  }
  vpl.close();
  surface_width = 0;
  surface_height = 0;
}

} // namespace onevpl
} // namespace jpeg
} // namespace oic
