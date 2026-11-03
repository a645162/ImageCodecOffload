/*
 * File: vpl_dyn.cpp
 * Brief: Vpl dyn.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// vpl_dyn.cpp - oneVPL 运行库动态加载实现（Linux dlopen / Windows LoadLibrary）。
#include "vpl_dyn.h"

#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace oic {
namespace jpeg {
namespace onevpl {

namespace {

// 发行版打包名优先：Ubuntu/Debian 的 libvpl2 只装 libvpl.so.2（不带 .so 开发链接）。
const char *kCandidates[] = {
#if defined(_WIN32)
    "libvpl.dll",
#else
    "libvpl.so.2",
    "libvpl.so",
#endif
};

void *platform_open(const char *name) {
#if defined(_WIN32)
  return static_cast<void *>(LoadLibraryA(name));
#else
  return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void platform_close(void *handle) {
#if defined(_WIN32)
  FreeLibrary(static_cast<HMODULE>(handle));
#else
  dlclose(handle);
#endif
}

std::string platform_error() {
#if defined(_WIN32)
  const DWORD code = GetLastError();
  return "win32 error " + std::to_string(static_cast<unsigned long>(code));
#else
  const char *err = dlerror();
  return err != nullptr ? std::string(err)
                        : std::string("unknown dlopen error");
#endif
}

void *platform_symbol(void *handle, const char *name) {
#if defined(_WIN32)
  return reinterpret_cast<void *>(
      GetProcAddress(static_cast<HMODULE>(handle), name));
#else
  return dlsym(handle, name);
#endif
}

// 绑定一个符号；失败直接把原因写进 error。
bool bind_symbol(void *handle, const char *name, void **slot,
                 std::string *error) {
  void *symbol = platform_symbol(handle, name);
  if (symbol == nullptr) {
    if (error != nullptr) {
      *error = std::string("运行库缺少符号 ") + name;
    }
    return false;
  }
  *slot = symbol;
  return true;
}

} // namespace

const char *vpl_candidate_library(int index) {
  const int count =
      static_cast<int>(sizeof(kCandidates) / sizeof(kCandidates[0]));
  if (index < 0 || index >= count) {
    return nullptr;
  }
  return kCandidates[index];
}

bool vpl_available() {
  VplDyn dyn;
  return dyn.load();
}

const char *vpl_status_str(mfxStatus sts) {
  switch (sts) {
  case MFX_ERR_NONE:
    return "MFX_ERR_NONE";
  case MFX_ERR_UNKNOWN:
    return "MFX_ERR_UNKNOWN";
  case MFX_ERR_UNSUPPORTED:
    return "MFX_ERR_UNSUPPORTED";
  case MFX_ERR_MEMORY_ALLOC:
    return "MFX_ERR_MEMORY_ALLOC";
  case MFX_ERR_NOT_ENOUGH_BUFFER:
    return "MFX_ERR_NOT_ENOUGH_BUFFER";
  case MFX_ERR_INVALID_HANDLE:
    return "MFX_ERR_INVALID_HANDLE";
  case MFX_ERR_LOCK_MEMORY:
    return "MFX_ERR_LOCK_MEMORY";
  case MFX_ERR_NOT_INITIALIZED:
    return "MFX_ERR_NOT_INITIALIZED";
  case MFX_ERR_NOT_FOUND:
    return "MFX_ERR_NOT_FOUND";
  case MFX_ERR_NULL_PTR:
    return "MFX_ERR_NULL_PTR";
  case MFX_ERR_UNDEFINED_BEHAVIOR:
    return "MFX_ERR_UNDEFINED_BEHAVIOR";
  case MFX_ERR_INVALID_VIDEO_PARAM:
    return "MFX_ERR_INVALID_VIDEO_PARAM";
  case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
    return "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM";
  case MFX_ERR_ABORTED:
    return "MFX_ERR_ABORTED";
  case MFX_ERR_MORE_DATA:
    return "MFX_ERR_MORE_DATA";
  case MFX_ERR_MORE_SURFACE:
    return "MFX_ERR_MORE_SURFACE";
  case MFX_WRN_IN_EXECUTION:
    return "MFX_WRN_IN_EXECUTION";
  case MFX_WRN_DEVICE_BUSY:
    return "MFX_WRN_DEVICE_BUSY";
  case MFX_WRN_VIDEO_PARAM_CHANGED:
    return "MFX_WRN_VIDEO_PARAM_CHANGED";
  case MFX_WRN_PARTIAL_ACCELERATION:
    return "MFX_WRN_PARTIAL_ACCELERATION";
  case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM:
    return "MFX_WRN_INCOMPATIBLE_VIDEO_PARAM";
  case MFX_WRN_VALUE_NOT_CHANGED:
    return "MFX_WRN_VALUE_NOT_CHANGED";
  case MFX_WRN_OUT_OF_RANGE:
    return "MFX_WRN_OUT_OF_RANGE";
  case MFX_WRN_FILTER_SKIPPED:
    return "MFX_WRN_FILTER_SKIPPED";
  default:
    return "MFX_STATUS_UNKNOWN";
  }
}

bool VplDyn::load(std::string *error) {
  unload();
  std::string last_error;
  for (int i = 0;; ++i) {
    const char *name = vpl_candidate_library(i);
    if (name == nullptr) {
      break;
    }
    void *handle = platform_open(name);
    if (handle == nullptr) {
      last_error = std::string("无法加载 ") + name + "：" + platform_error();
      continue;
    }
    h_dll = handle;
    library_path = name;
    break;
  }
  if (h_dll == nullptr) {
    if (error != nullptr) {
      *error =
          last_error.empty() ? std::string("未找到 oneVPL 运行库") : last_error;
    }
    return false;
  }

  // 逐项绑定；任何一个失败都视为不可用（缺符号时宁可明确失败，也不要
  // 在运行到一半时空指针跳转）。
#define OIC_VPL_BIND(name)                                                     \
  do {                                                                         \
    void **slot = reinterpret_cast<void **>(&this->name);                      \
    if (!bind_symbol(h_dll, #name, slot, error)) {                             \
      unload();                                                                \
      return false;                                                            \
    }                                                                          \
  } while (0)

  OIC_VPL_BIND(MFXLoad);
  OIC_VPL_BIND(MFXUnload);
  OIC_VPL_BIND(MFXCreateConfig);
  OIC_VPL_BIND(MFXSetConfigFilterProperty);
  OIC_VPL_BIND(MFXEnumImplementations);
  OIC_VPL_BIND(MFXDispReleaseImplDescription);
  OIC_VPL_BIND(MFXCreateSession);
  OIC_VPL_BIND(MFXClose);
  OIC_VPL_BIND(MFXMemory_GetSurfaceForEncode);
  OIC_VPL_BIND(MFXMemory_GetSurfaceForDecode);
  OIC_VPL_BIND(MFXVideoENCODE_Query);
  OIC_VPL_BIND(MFXVideoENCODE_QueryIOSurf);
  OIC_VPL_BIND(MFXVideoENCODE_Init);
  OIC_VPL_BIND(MFXVideoENCODE_EncodeFrameAsync);
  OIC_VPL_BIND(MFXVideoENCODE_Close);
  OIC_VPL_BIND(MFXVideoDECODE_DecodeHeader);
  OIC_VPL_BIND(MFXVideoDECODE_Query);
  OIC_VPL_BIND(MFXVideoDECODE_Init);
  OIC_VPL_BIND(MFXVideoDECODE_DecodeFrameAsync);
  OIC_VPL_BIND(MFXVideoDECODE_Close);
  OIC_VPL_BIND(MFXVideoCORE_SyncOperation);
  OIC_VPL_BIND(MFXVideoCORE_SetHandle);
  OIC_VPL_BIND(MFXVideoCORE_SetFrameAllocator);

#undef OIC_VPL_BIND
  return true;
}

void VplDyn::unload() {
  if (h_dll != nullptr) {
    platform_close(h_dll);
    h_dll = nullptr;
  }
  library_path.clear();
  MFXLoad = nullptr;
  MFXUnload = nullptr;
  MFXCreateConfig = nullptr;
  MFXSetConfigFilterProperty = nullptr;
  MFXEnumImplementations = nullptr;
  MFXDispReleaseImplDescription = nullptr;
  MFXCreateSession = nullptr;
  MFXClose = nullptr;
  MFXMemory_GetSurfaceForEncode = nullptr;
  MFXMemory_GetSurfaceForDecode = nullptr;
  MFXVideoENCODE_Query = nullptr;
  MFXVideoENCODE_QueryIOSurf = nullptr;
  MFXVideoENCODE_Init = nullptr;
  MFXVideoENCODE_EncodeFrameAsync = nullptr;
  MFXVideoENCODE_Close = nullptr;
  MFXVideoDECODE_DecodeHeader = nullptr;
  MFXVideoDECODE_Query = nullptr;
  MFXVideoDECODE_Init = nullptr;
  MFXVideoDECODE_DecodeFrameAsync = nullptr;
  MFXVideoDECODE_Close = nullptr;
  MFXVideoCORE_SyncOperation = nullptr;
  MFXVideoCORE_SetHandle = nullptr;
  MFXVideoCORE_SetFrameAllocator = nullptr;
}

} // namespace onevpl
} // namespace jpeg
} // namespace oic
