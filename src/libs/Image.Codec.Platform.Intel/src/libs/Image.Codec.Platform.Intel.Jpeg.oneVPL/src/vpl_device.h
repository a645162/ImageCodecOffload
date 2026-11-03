/*
 * File: vpl_device.h
 * Brief: Vpl device.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// vpl_device.h - Intel 核显硬件单元（oneVPL 实现）的枚举与会话建立。
//
// 一个「硬件单元」就是 oneVPL 运行库里的一条实现（mfxImplDescription）：
// Intel 核显的媒体引擎（MFX / VPL GPU Runtime，即 vpl-gpu-rt · libmfx-gen）
// 会以 VendorID=0x8086、Impl=MFX_IMPL_TYPE_HARDWARE 的形式出现。
//
// 这一层存在的意义：
//  - 把「运行库没装 / 装了但没有实现 / 有实现但不支持 JPEG」这三种失败分开，
//    上层才能给出可操作的提示，而不是一句 encode failed；
//  - 让调用方可以指定使用第几条实现（多显卡机器上可以选核显而不是独显）；
//  - 统一会话建立路径，编码器/解码器不再各写一份枚举代码。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vpl_dyn.h"

namespace oic {
namespace jpeg {
namespace onevpl {

// 一条 oneVPL 实现（硬件单元 / 软件实现）。
struct VplUnitInfo {
  int index = -1; // MFXEnumImplementations 的下标，建会话时原样使用
  bool hardware = false;
  uint32_t vendor_id = 0;
  uint32_t vendor_impl_id = 0;
  uint16_t api_version_major = 0;
  uint16_t api_version_minor = 0;
  uint32_t acceleration_mode = 0;
  std::string impl_name;
  std::string license;
  std::string keywords;
  std::string device_id;
  bool jpeg_decode = false;
  bool jpeg_encode = false;
  std::vector<uint32_t> decode_fourcc;
  std::vector<uint32_t> encode_fourcc;

  bool is_intel() const { return vendor_id == 0x8086; }
  // 厂商名（未知厂商返回十六进制）。
  std::string vendor_name() const;
  // 加速栈（VA-API / D3D11 / D3D9 / NA …）。
  std::string acceleration_name() const;
  // 一行可读描述，CLI 直接打印。
  std::string describe() const;
};

// 一次探测的完整结果。
struct VplProbeResult {
  bool loader_available = false;
  std::string library; // 实际加载到的运行库名
  std::string error;   // 加载 / 枚举失败的原因（成功时为空）
  std::vector<VplUnitInfo> units;

  // 是否存在 Intel 硬件单元（不管它支不支持 JPEG）。
  bool has_intel_hardware() const;
  // 是否存在「Intel 硬件 + 支持 JPEG 编码/解码」的单元。
  bool has_intel_jpeg_encoder() const;
  bool has_intel_jpeg_decoder() const;
  // 多行可读报告，包含「没装运行时」与「装了但没有实现」的区别。
  std::string describe() const;
};

// 探测当前机器：加载运行库、枚举全部实现、解析 JPEG 能力。
// 每次调用都会独立加载/卸载运行库，可安全重复调用。
VplProbeResult vpl_probe();

// FourCC -> 可读字符串（NV12 / I420 / ...），未知值输出 0x........。
std::string vpl_fourcc_str(uint32_t fourcc);

// 一个已建立的会话（RAII）：持有运行库、loader、session 与所选单元。
struct VplSession {
  VplDyn dyn;
  mfxLoader loader = nullptr;
  mfxSession session = nullptr;
  VplUnitInfo unit;

  VplSession() = default;
  VplSession(const VplSession &) = delete;
  VplSession &operator=(const VplSession &) = delete;
  ~VplSession() { close(); }

  // 按指定单元建会话。失败时 error 说明是哪一步。
  bool open(const VplUnitInfo &selected, std::string *error = nullptr);
  // 选一条 Intel 硬件单元建会话（require_encode/decode 决定能力门槛）。
  bool open_intel(bool require_encode, bool require_decode,
                  std::string *error = nullptr);
  void close();
};

} // namespace onevpl
} // namespace jpeg
} // namespace oic
