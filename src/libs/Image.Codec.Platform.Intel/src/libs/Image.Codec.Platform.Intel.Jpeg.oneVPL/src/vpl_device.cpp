/*
 * File: vpl_device.cpp
 * Brief: Vpl device.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// vpl_device.cpp - Intel 核显硬件单元的枚举与会话建立实现。
#include "vpl_device.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace oic {
namespace jpeg {
namespace onevpl {

namespace {

std::string fourcc_to_string(uint32_t fourcc) {
  if (fourcc == 0) {
    return "none";
  }
  char text[5] = {0, 0, 0, 0, 0};
  text[0] = static_cast<char>(fourcc & 0xFF);
  text[1] = static_cast<char>((fourcc >> 8) & 0xFF);
  text[2] = static_cast<char>((fourcc >> 16) & 0xFF);
  text[3] = static_cast<char>((fourcc >> 24) & 0xFF);
  bool printable = true;
  for (int i = 0; i < 4; ++i) {
    if (text[i] < 0x20 || text[i] > 0x7E) {
      printable = false;
      break;
    }
  }
  if (!printable) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08X", fourcc);
    return hex;
  }
  return std::string(text, 4);
}

template <typename T>
void append_codec_colors(T &codec, std::vector<uint32_t> &colors) {
  for (mfxU16 p = 0; p < codec.NumProfiles; ++p) {
    auto &profile = codec.Profiles[p];
    for (mfxU16 m = 0; m < profile.NumMemTypes; ++m) {
      auto &mem = profile.MemDesc[m];
      for (mfxU16 c = 0; c < mem.NumColorFormats; ++c) {
        const uint32_t fourcc = static_cast<uint32_t>(mem.ColorFormats[c]);
        // 同一个 profile 会为系统内存与加速栈内存各列一遍格式
        // （Windows 是 D3D11、Linux 是 VA-API），报告的格式列表按去重展示，
        // 否则 enc_in 里每种格式出现两次，看起来像能力翻倍。
        if (std::find(colors.begin(), colors.end(), fourcc) == colors.end()) {
          colors.push_back(fourcc);
        }
      }
    }
  }
}

void collect_caps(const mfxImplDescription &desc, VplUnitInfo &unit) {
  for (mfxU16 i = 0; i < desc.Dec.NumCodecs; ++i) {
    if (desc.Dec.Codecs[i].CodecID != MFX_CODEC_JPEG) {
      continue;
    }
    unit.jpeg_decode = true;
    append_codec_colors(desc.Dec.Codecs[i], unit.decode_fourcc);
  }
  for (mfxU16 i = 0; i < desc.Enc.NumCodecs; ++i) {
    if (desc.Enc.Codecs[i].CodecID != MFX_CODEC_JPEG) {
      continue;
    }
    unit.jpeg_encode = true;
    append_codec_colors(desc.Enc.Codecs[i], unit.encode_fourcc);
  }
}

const char *runtime_hint() {
#if defined(_WIN32)
  return "Intel 核显的媒体引擎需要 vpl-gpu-rt（安装包里的 libmfx-gen.dll），"
         "请安装 Intel 显卡驱动或 vpl-gpu-rt。";
#else
  return "Intel 核显的媒体引擎需要 vpl-gpu-rt（libmfx-gen.so.1），"
         "请安装 intel-media-driver / vpl-gpu-rt。";
#endif
}

} // namespace

std::string VplUnitInfo::vendor_name() const {
  if (vendor_id == 0x8086) {
    return "Intel";
  }
  if (vendor_id == 0x10DE) {
    return "NVIDIA";
  }
  if (vendor_id == 0x1002) {
    return "AMD";
  }
  char text[16];
  std::snprintf(text, sizeof(text), "0x%04X", vendor_id);
  return text;
}

std::string VplUnitInfo::acceleration_name() const {
  switch (acceleration_mode) {
  case MFX_ACCEL_MODE_NA:
    return "NA";
  case MFX_ACCEL_MODE_VIA_D3D9:
    return "D3D9";
  case MFX_ACCEL_MODE_VIA_D3D11:
    return "D3D11";
  case MFX_ACCEL_MODE_VIA_VAAPI:
    return "VA-API";
  case MFX_ACCEL_MODE_VIA_VAAPI_DRM_MODESET:
    return "VA-API(DRM modeset)";
  case MFX_ACCEL_MODE_VIA_VAAPI_GLX:
    return "VA-API(GLX)";
  case MFX_ACCEL_MODE_VIA_VAAPI_X11:
    return "VA-API(X11)";
  case MFX_ACCEL_MODE_VIA_VAAPI_WAYLAND:
    return "VA-API(Wayland)";
  case MFX_ACCEL_MODE_VIA_HDDLUNITE:
    return "HDDLUNITE";
  default:
    break;
  }
  char text[24];
  std::snprintf(text, sizeof(text), "0x%X", acceleration_mode);
  return text;
}

std::string VplUnitInfo::describe() const {
  std::string text;
  text += "#" + std::to_string(index);
  text += " ";
  text += impl_name.empty() ? "unknown" : impl_name;
  text += hardware ? " [hardware]" : " [software]";
  text += " vendor=" + vendor_name();
  if (!device_id.empty()) {
    text += " device=" + device_id;
  }
  text += " accel=" + acceleration_name();
  text += " api=" + std::to_string(api_version_major) + "." +
          std::to_string(api_version_minor);
  text += " jpeg_encode=";
  text += jpeg_encode ? "yes" : "no";
  text += " jpeg_decode=";
  text += jpeg_decode ? "yes" : "no";
  if (!encode_fourcc.empty() || !decode_fourcc.empty()) {
    text += " enc_in=[";
    for (size_t i = 0; i < encode_fourcc.size(); ++i) {
      if (i != 0) {
        text += ",";
      }
      text += fourcc_to_string(encode_fourcc[i]);
    }
    text += "] dec_out=[";
    for (size_t i = 0; i < decode_fourcc.size(); ++i) {
      if (i != 0) {
        text += ",";
      }
      text += fourcc_to_string(decode_fourcc[i]);
    }
    text += "]";
  }
  return text;
}

bool VplProbeResult::has_intel_hardware() const {
  for (const auto &unit : units) {
    if (unit.is_intel() && unit.hardware) {
      return true;
    }
  }
  return false;
}

bool VplProbeResult::has_intel_jpeg_encoder() const {
  for (const auto &unit : units) {
    if (unit.is_intel() && unit.hardware && unit.jpeg_encode) {
      return true;
    }
  }
  return false;
}

bool VplProbeResult::has_intel_jpeg_decoder() const {
  for (const auto &unit : units) {
    if (unit.is_intel() && unit.hardware && unit.jpeg_decode) {
      return true;
    }
  }
  return false;
}

std::string VplProbeResult::describe() const {
  std::string text;
  if (!loader_available) {
    text += "oneVPL 运行库：不可用\n";
    text += "  原因：" + error + "\n";
    text += std::string("  提示：") + runtime_hint();
    return text;
  }
  text += "oneVPL 运行库：" + library + "\n";
  text += "实现数量：" + std::to_string(units.size()) + "\n";
  if (units.empty()) {
    text += "  未找到任何 oneVPL 实现。\n";
    text += std::string("  提示：") + runtime_hint();
    return text;
  }
  for (const auto &unit : units) {
    text += "  " + unit.describe() + "\n";
  }
  if (!has_intel_hardware()) {
    text += "  Intel 硬件单元：无\n";
    text += std::string("  提示：") + runtime_hint();
  } else {
    text += "  Intel 硬件单元：有（JPEG 编码=";
    text += has_intel_jpeg_encoder() ? "yes" : "no";
    text += "，解码=";
    text += has_intel_jpeg_decoder() ? "yes" : "no";
    text += "）\n";
  }
  return text;
}

std::string vpl_fourcc_str(uint32_t fourcc) { return fourcc_to_string(fourcc); }

VplProbeResult vpl_probe() {
  VplProbeResult result;
  VplDyn dyn;
  if (!dyn.load(&result.error)) {
    return result;
  }
  result.loader_available = true;
  result.library = dyn.library_path;

  mfxLoader loader = dyn.MFXLoad();
  if (loader == nullptr) {
    result.error = "MFXLoad 返回空 loader";
    return result;
  }

  // 不加任何过滤：软件实现也要能看到，否则「没装实现」与「实现不支持硬件」
  // 会被混成同一个空列表。
  for (mfxU32 i = 0;; ++i) {
    mfxHDL handle = nullptr;
    const mfxStatus sts = dyn.MFXEnumImplementations(
        loader, i, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &handle);
    if (sts != MFX_ERR_NONE || handle == nullptr) {
      break;
    }
    auto *desc = static_cast<mfxImplDescription *>(handle);
    VplUnitInfo unit;
    unit.index = static_cast<int>(i);
    unit.hardware = desc->Impl == MFX_IMPL_TYPE_HARDWARE;
    unit.vendor_id = desc->VendorID;
    unit.vendor_impl_id = desc->VendorImplID;
    unit.api_version_major = desc->ApiVersion.Major;
    unit.api_version_minor = desc->ApiVersion.Minor;
    unit.acceleration_mode = static_cast<uint32_t>(desc->AccelerationMode);
    unit.impl_name = desc->ImplName;
    unit.license = desc->License;
    unit.keywords = desc->Keywords;
    if (desc->Dev.DeviceID[0] != '\0') {
      unit.device_id = desc->Dev.DeviceID;
    }
    collect_caps(*desc, unit);
    result.units.push_back(unit);
    dyn.MFXDispReleaseImplDescription(loader, handle);
  }

  dyn.MFXUnload(loader);
  return result;
}

bool VplSession::open(const VplUnitInfo &selected, std::string *error) {
  close();
  if (!dyn.load(error)) {
    return false;
  }
  loader = dyn.MFXLoad();
  if (loader == nullptr) {
    if (error != nullptr) {
      *error = "MFXLoad 返回空 loader";
    }
    close();
    return false;
  }
  const mfxStatus sts = dyn.MFXCreateSession(
      loader, static_cast<mfxU32>(selected.index), &session);
  if (sts != MFX_ERR_NONE || session == nullptr) {
    if (error != nullptr) {
      *error = "MFXCreateSession(" + selected.impl_name + ") 失败：" +
               vpl_status_str(sts);
    }
    close();
    return false;
  }
  unit = selected;
  return true;
}

bool VplSession::open_intel(bool require_encode, bool require_decode,
                            std::string *error) {
  const VplProbeResult probe = vpl_probe();
  if (!probe.loader_available) {
    if (error != nullptr) {
      *error = probe.error;
    }
    return false;
  }
  // 优先硬件 + Intel + 能力齐备；退而求其次只在同厂商内放宽能力，
  // 绝不跨到软件实现（那会让「硬件编码」变成一句谎话）。
  const VplUnitInfo *chosen = nullptr;
  for (const auto &candidate : probe.units) {
    if (!candidate.is_intel() || !candidate.hardware) {
      continue;
    }
    if (require_encode && !candidate.jpeg_encode) {
      continue;
    }
    if (require_decode && !candidate.jpeg_decode) {
      continue;
    }
    chosen = &candidate;
    break;
  }
  if (chosen == nullptr) {
    if (error != nullptr) {
      std::string need;
      if (require_encode) {
        need += "编码";
      }
      if (require_decode) {
        need += need.empty() ? "解码" : "/解码";
      }
      if (probe.has_intel_hardware()) {
        *error = "找到 Intel 硬件单元，但它不支持 JPEG " + need +
                 "（可能缺少媒体引擎）";
      } else {
        *error = "未找到 Intel 硬件单元：";
        *error += runtime_hint();
      }
    }
    return false;
  }
  return open(*chosen, error);
}

void VplSession::close() {
  if (session != nullptr) {
    dyn.MFXClose(session);
    session = nullptr;
  }
  if (loader != nullptr) {
    dyn.MFXUnload(loader);
    loader = nullptr;
  }
  dyn.unload();
  unit = VplUnitInfo{};
}

} // namespace onevpl
} // namespace jpeg
} // namespace oic
