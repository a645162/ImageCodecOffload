/*
 * File: vpl_dyn.h
 * Brief: Vpl dyn.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// vpl_dyn.h - oneVPL 运行库的跨平台动态加载绑定。
//
// Linux 用 dlopen，Windows 用 LoadLibrary；两种平台都不需要导入库。
// 运行库只提供「加载器 / 会话 / 编解码入口」，真正的硬件实现由运行库在
// 运行期自行 dlopen（Intel 核显需要安装 vpl-gpu-rt，即 libmfx-gen）。
//
// 头文件来自 libvpl 子模块（3rdparty/libvpl/api/vpl），未做任何复制。
#pragma once

#include <string>

#include "mfxdispatcher.h"
#include "mfxjpeg.h"
#include "mfxmemory.h"
#include "mfxvideo.h"

namespace oic {
namespace jpeg {
namespace onevpl {

// 候选运行库名（按尝试顺序）。Linux: libvpl.so.2 -> libvpl.so；
// Windows: libvpl.dll。没有候选时返回 nullptr。
const char *vpl_candidate_library(int index);

// 运行库是否存在且全部必需符号可解析（轻量探测，加载后立即卸载）。
bool vpl_available();

// mfxStatus -> 可读字符串（用于错误上报）。
const char *vpl_status_str(mfxStatus sts);

// 动态加载运行库并绑定 MFX 入口。一次 load() 绑定全部用到的符号，
// 失败时已 unload 并把原因写进 error。
struct VplDyn {
  void *h_dll = nullptr;
  std::string library_path; // 实际加载成功的候选名

  // loader / session（oneVPL dispatcher 风格）
  mfxLoader(MFX_CDECL *MFXLoad)() = nullptr;
  void(MFX_CDECL *MFXUnload)(mfxLoader) = nullptr;
  mfxConfig(MFX_CDECL *MFXCreateConfig)(mfxLoader) = nullptr;
  mfxStatus(MFX_CDECL *MFXSetConfigFilterProperty)(mfxConfig, const mfxU8 *,
                                                   mfxVariant) = nullptr;
  mfxStatus(MFX_CDECL *MFXEnumImplementations)(mfxLoader, mfxU32,
                                               mfxImplCapsDeliveryFormat,
                                               mfxHDL *) = nullptr;
  mfxStatus(MFX_CDECL *MFXDispReleaseImplDescription)(mfxLoader,
                                                      mfxHDL) = nullptr;
  mfxStatus(MFX_CDECL *MFXCreateSession)(mfxLoader, mfxU32,
                                         mfxSession *) = nullptr;
  mfxStatus(MFX_CDECL *MFXClose)(mfxSession) = nullptr;

  // 内部帧分配（系统内存路径）：不依赖 D3D11 / VA-API 句柄。
  mfxStatus(MFX_CDECL *MFXMemory_GetSurfaceForEncode)(
      mfxSession, mfxFrameSurface1 **) = nullptr;
  mfxStatus(MFX_CDECL *MFXMemory_GetSurfaceForDecode)(
      mfxSession, mfxFrameSurface1 **) = nullptr;

  // encode
  mfxStatus(MFX_CDECL *MFXVideoENCODE_Query)(mfxSession, mfxVideoParam *,
                                             mfxVideoParam *) = nullptr;
  mfxStatus(MFX_CDECL *MFXVideoENCODE_QueryIOSurf)(
      mfxSession, mfxVideoParam *, mfxFrameAllocRequest *) = nullptr;
  mfxStatus(MFX_CDECL *MFXVideoENCODE_Init)(mfxSession,
                                            mfxVideoParam *) = nullptr;
  mfxStatus(MFX_CDECL *MFXVideoENCODE_EncodeFrameAsync)(
      mfxSession, mfxEncodeCtrl *, mfxFrameSurface1 *, mfxBitstream *,
      mfxSyncPoint *) = nullptr;
  mfxStatus(MFX_CDECL *MFXVideoENCODE_Close)(mfxSession) = nullptr;

  // decode
  mfxStatus(MFX_CDECL *MFXVideoDECODE_DecodeHeader)(mfxSession, mfxBitstream *,
                                                    mfxVideoParam *) = nullptr;
  mfxStatus(MFX_CDECL *MFXVideoDECODE_Query)(mfxSession, mfxVideoParam *,
                                             mfxVideoParam *) = nullptr;
  mfxStatus(MFX_CDECL *MFXVideoDECODE_Init)(mfxSession,
                                            mfxVideoParam *) = nullptr;
  mfxStatus(MFX_CDECL *MFXVideoDECODE_DecodeFrameAsync)(
      mfxSession, mfxBitstream *, mfxFrameSurface1 *, mfxFrameSurface1 **,
      mfxSyncPoint *) = nullptr;
  mfxStatus(MFX_CDECL *MFXVideoDECODE_Close)(mfxSession) = nullptr;

  // core
  mfxStatus(MFX_CDECL *MFXVideoCORE_SyncOperation)(mfxSession, mfxSyncPoint,
                                                   mfxU32) = nullptr;
  mfxStatus(MFX_CDECL *MFXVideoCORE_SetHandle)(mfxSession, mfxHandleType,
                                               mfxHDL) = nullptr;
  mfxStatus(MFX_CDECL *MFXVideoCORE_SetFrameAllocator)(
      mfxSession, mfxFrameAllocator *) = nullptr;

  VplDyn() = default;
  VplDyn(const VplDyn &) = delete;
  VplDyn &operator=(const VplDyn &) = delete;
  ~VplDyn() { unload(); }

  bool load(std::string *error = nullptr);
  void unload();
  bool loaded() const { return h_dll != nullptr; }
};

} // namespace onevpl
} // namespace jpeg
} // namespace oic
