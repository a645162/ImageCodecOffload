/*
 * File: nvjpeg2k_codec.cpp
 * Brief: JPEG 2000 codec on nvJPEG2000.
 * Author: Haomin Kong
 * Email: konghm@beim.com.cn / a645162@163.com
 * Created: 2026-11-03
 */

// nvjpeg2k_codec.cpp - nvJPEG2000 编解码实现。
#include "nvjpeg2k_codec.h"

#include <algorithm>

#include "nvidia_error.h"

#if defined(OIC_NVIDIA_HAS_NVJPEG2K)
#include <nvjpeg2k.h>
#endif

namespace image_codec {
namespace nvidia {
namespace jpeg2k {

namespace {

#if defined(OIC_NVIDIA_HAS_NVJPEG2K)

std::string status_text(nvjpeg2kStatus_t st) {
  switch (st) {
  case NVJPEG2K_STATUS_SUCCESS:
    return "success";
  case NVJPEG2K_STATUS_NOT_INITIALIZED:
    return "nvJPEG2000 not initialized";
  case NVJPEG2K_STATUS_INVALID_PARAMETER:
    return "invalid parameter";
  case NVJPEG2K_STATUS_BAD_JPEG:
    return "bad JPEG 2000 bitstream";
  case NVJPEG2K_STATUS_JPEG_NOT_SUPPORTED:
    return "JPEG 2000 not supported for this configuration";
  case NVJPEG2K_STATUS_ALLOCATOR_FAILURE:
    return "allocator failure";
  case NVJPEG2K_STATUS_EXECUTION_FAILED:
    return "execution failed";
  case NVJPEG2K_STATUS_ARCH_MISMATCH:
    return "architecture mismatch";
  case NVJPEG2K_STATUS_INTERNAL_ERROR:
    return "internal error";
  case NVJPEG2K_STATUS_IMPLEMENTATION_NOT_SUPPORTED:
    return "implementation not supported";
  case NVJPEG2K_STATUS_TILE_TOO_LARGE:
    return "tile part exceeds 4 GB (re-encode with smaller tiles)";
  default:
    return "nvJPEG2000 status " + std::to_string(static_cast<int>(st));
  }
}

#define OIC_NVJPEG2K_OK(call, what)                                            \
  do {                                                                         \
    const nvjpeg2kStatus_t _st = (call);                                       \
    if (_st != NVJPEG2K_STATUS_SUCCESS) {                                      \
      if (err != nullptr) {                                                    \
        *err = std::string(what) + ": " + status_text(_st);                    \
      }                                                                        \
      return false;                                                            \
    }                                                                          \
  } while (0)

// 每个厂商句柄一个 RAII，保证失败路径也不会漏掉销毁。
template <typename T, nvjpeg2kStatus_t (*Destroy)(T)> struct HandleGuard {
  T handle = nullptr;
  ~HandleGuard() {
    if (handle != nullptr) (void)Destroy(handle);
  }
};

using HandleGuardLibrary = HandleGuard<nvjpeg2kHandle_t, nvjpeg2kDestroy>;
using HandleGuardStream = HandleGuard<nvjpeg2kStream_t, nvjpeg2kStreamDestroy>;
using HandleGuardDecodeState =
    HandleGuard<nvjpeg2kDecodeState_t, nvjpeg2kDecodeStateDestroy>;
using HandleGuardDecodeParams =
    HandleGuard<nvjpeg2kDecodeParams_t, nvjpeg2kDecodeParamsDestroy>;
using HandleGuardEncoder =
    HandleGuard<nvjpeg2kEncoder_t, nvjpeg2kEncoderDestroy>;
using HandleGuardEncodeState =
    HandleGuard<nvjpeg2kEncodeState_t, nvjpeg2kEncodeStateDestroy>;
using HandleGuardEncodeParams =
    HandleGuard<nvjpeg2kEncodeParams_t, nvjpeg2kEncodeParamsDestroy>;

// pitch 分配 + 显存记账的 RAII（nvJPEG2000 的 pitch 必须是样本字节数的整数倍）。
struct PitchBuffer {
  void *ptr = nullptr;
  size_t pitch = 0;
  size_t bytes = 0;
  MemoryBudget *budget = nullptr;

  ~PitchBuffer() {
    if (ptr != nullptr) {
      (void)cudaFree(ptr);
      if (budget != nullptr) budget->release(bytes);
    }
  }

  bool allocate(size_t row_bytes, uint32_t height, std::string *err) {
    bytes = row_bytes * static_cast<size_t>(height);
    if (budget != nullptr && !budget->reserve(bytes, err)) {
      return false;
    }
    if (!check_cuda(cudaMallocPitch(&ptr, &pitch, row_bytes, height),
                    "cudaMallocPitch", err)) {
      if (budget != nullptr) budget->release(bytes);
      bytes = 0;
      return false;
    }
    return true;
  }
};

// 分辨率级数：coarsest 层至少 1x1，且不超过 nvJPEG2000 的 33 级上限。
uint32_t resolution_levels(uint32_t width, uint32_t height) {
  const uint32_t min_dim = std::min(width, height);
  uint32_t levels = 1;
  while (levels < 33 && (1ull << levels) <= min_dim) {
    ++levels;
  }
  return levels;
}

#endif // OIC_NVIDIA_HAS_NVJPEG2K

} // namespace

bool available() {
#if defined(OIC_NVIDIA_HAS_NVJPEG2K)
  return true;
#else
  return false;
#endif
}

std::string version() {
#if defined(OIC_NVIDIA_HAS_NVJPEG2K)
  int major = 0;
  int minor = 0;
  if (nvjpeg2kGetProperty(MAJOR_VERSION, &major) != NVJPEG2K_STATUS_SUCCESS ||
      nvjpeg2kGetProperty(MINOR_VERSION, &minor) != NVJPEG2K_STATUS_SUCCESS) {
    return {};
  }
  return std::to_string(major) + "." + std::to_string(minor);
#else
  return {};
#endif
}

bool decode(int device_index, const uint8_t *data, size_t size,
            ImageBuffer *out, std::string *err, MemoryBudget *budget) {
#if !defined(OIC_NVIDIA_HAS_NVJPEG2K)
  (void)device_index;
  (void)data;
  (void)size;
  (void)out;
  (void)budget;
  if (err != nullptr) *err = "nvJPEG2000 is not available in this build";
  return false;
#else
  if (data == nullptr || size == 0 || out == nullptr) {
    if (err != nullptr) *err = "JPEG2000: empty input";
    return false;
  }
  Device dev;
  if (!dev.init(device_index, err)) {
    return false;
  }

  HandleGuardLibrary lib;
  OIC_NVJPEG2K_OK(nvjpeg2kCreateSimple(&lib.handle), "nvjpeg2kCreateSimple");

  HandleGuardStream stream;
  OIC_NVJPEG2K_OK(nvjpeg2kStreamCreate(&stream.handle), "nvjpeg2kStreamCreate");
  OIC_NVJPEG2K_OK(
      nvjpeg2kStreamParse(lib.handle, data, size, 0, 0, stream.handle),
      "nvjpeg2kStreamParse");

  nvjpeg2kImageInfo_t info{};
  OIC_NVJPEG2K_OK(nvjpeg2kStreamGetImageInfo(stream.handle, &info),
                  "nvjpeg2kStreamGetImageInfo");
  if (info.image_width == 0 || info.image_height == 0) {
    if (err != nullptr) *err = "JPEG2000: zero image dimension";
    return false;
  }
  // 通道保真：只接受 1（灰度）与 3（彩色）。2/4 通道的 JPEG2000 不在本项目的
  // 图像模型里（灰度+A / RGBA 需要逐通道语义），明确拒绝而不是按 3 通道截断。
  if (info.num_components != 1 && info.num_components != 3) {
    if (err != nullptr) {
      *err = "JPEG2000: " + std::to_string(info.num_components) +
             "-component images are not supported (only 1 or 3)";
    }
    return false;
  }

  uint8_t precision = 0;
  for (uint32_t c = 0; c < info.num_components; ++c) {
    nvjpeg2kImageComponentInfo_t comp{};
    OIC_NVJPEG2K_OK(
        nvjpeg2kStreamGetImageComponentInfo(stream.handle, &comp, c),
        "nvjpeg2kStreamGetImageComponentInfo");
    if (comp.sgn != 0) {
      if (err != nullptr) *err = "JPEG2000: signed components are unsupported";
      return false;
    }
    if (c == 0) {
      precision = comp.precision;
    } else if (comp.precision != precision) {
      if (err != nullptr)
        *err = "JPEG2000: components with different bit depths are unsupported";
      return false;
    }
  }
  if (precision == 0 || precision > 16) {
    if (err != nullptr) {
      *err = "JPEG2000: unsupported component precision " +
             std::to_string(static_cast<int>(precision));
    }
    return false;
  }
  const bool sixteen = precision > 8;
  const uint32_t bytes_per_sample = sixteen ? 2 : 1;
  const int out_channels = static_cast<int>(info.num_components);
  const size_t row_bytes = static_cast<size_t>(info.image_width) *
                           static_cast<size_t>(out_channels) *
                           static_cast<size_t>(bytes_per_sample);

  HandleGuardDecodeParams params;
  OIC_NVJPEG2K_OK(nvjpeg2kDecodeParamsCreate(&params.handle),
                  "nvjpeg2kDecodeParamsCreate");
  // 交错布局让宿主侧一次拷贝就得到目标内存序；3 通道再打开 RGB 输出，把
  // 4:2:2 / 4:2:0 的子采样色度在设备上上采样到亮度网格。
  OIC_NVJPEG2K_OK(nvjpeg2kDecodeParamsSetOutputFormat(
                      params.handle, NVJPEG2K_FORMAT_INTERLEAVED),
                  "nvjpeg2kDecodeParamsSetOutputFormat");
  if (out_channels == 3) {
    OIC_NVJPEG2K_OK(nvjpeg2kDecodeParamsSetRGBOutput(params.handle, 1),
                    "nvjpeg2kDecodeParamsSetRGBOutput");
  }

  PitchBuffer buffer;
  buffer.budget = budget;
  if (!buffer.allocate(row_bytes, info.image_height, err)) {
    return false;
  }

  HandleGuardDecodeState state;
  OIC_NVJPEG2K_OK(nvjpeg2kDecodeStateCreate(lib.handle, &state.handle),
                  "nvjpeg2kDecodeStateCreate");

  nvjpeg2kImage_t dst{};
  dst.pixel_data = &buffer.ptr;
  dst.pitch_in_bytes = &buffer.pitch;
  dst.pixel_type = sixteen ? NVJPEG2K_UINT16 : NVJPEG2K_UINT8;
  dst.num_components = info.num_components;

  OIC_NVJPEG2K_OK(nvjpeg2kDecodeImage(lib.handle, state.handle, stream.handle,
                                      params.handle, &dst, dev.stream()),
                  "nvjpeg2kDecodeImage");
  if (!dev.synchronize(err)) {
    return false;
  }

  if (!out->allocate(static_cast<int>(info.image_width),
                     static_cast<int>(info.image_height), out_channels)) {
    if (err != nullptr) *err = "JPEG2000: allocation failed";
    return false;
  }

  // 逐行读出并（必要时）把 16 bit 右移到 8 bit。逐行是为了吃掉 pitch padding。
  const size_t out_row = out->stride();
  if (!sixteen) {
    for (uint32_t y = 0; y < info.image_height; ++y) {
      if (!download(dev.stream(),
                    out->data.data() + static_cast<size_t>(y) * out_row,
                    static_cast<const uint8_t *>(buffer.ptr) +
                        static_cast<size_t>(y) * buffer.pitch,
                    row_bytes, err)) {
        return false;
      }
    }
  } else {
    const uint32_t shift = static_cast<uint32_t>(precision - 8);
    std::vector<uint16_t> row(static_cast<size_t>(info.image_width) *
                              static_cast<size_t>(out_channels));
    for (uint32_t y = 0; y < info.image_height; ++y) {
      if (!download(dev.stream(), row.data(),
                    static_cast<const uint8_t *>(buffer.ptr) +
                        static_cast<size_t>(y) * buffer.pitch,
                    row_bytes, err)) {
        return false;
      }
      // 必须同步后才能读 row——download() 只是把拷贝排进 stream。
      if (!dev.synchronize(err)) {
        return false;
      }
      uint8_t *dst_row = out->data.data() + static_cast<size_t>(y) * out_row;
      for (size_t i = 0; i < row.size(); ++i) {
        dst_row[i] = static_cast<uint8_t>(row[i] >> shift);
      }
    }
  }
  return dev.synchronize(err);
#endif
}

bool encode(int device_index, const ImageBuffer &img, int quality,
            std::vector<uint8_t> *out, std::string *err, MemoryBudget *budget) {
#if !defined(OIC_NVIDIA_HAS_NVJPEG2K)
  (void)device_index;
  (void)img;
  (void)quality;
  (void)out;
  (void)budget;
  if (err != nullptr) *err = "nvJPEG2000 is not available in this build";
  return false;
#else
  if (out == nullptr) {
    if (err != nullptr) *err = "JPEG2000: null output";
    return false;
  }
  out->clear();
  if (!img.valid() || (img.channels != 1 && img.channels != 3)) {
    if (err != nullptr)
      *err = "JPEG2000: only 1/3-channel 8-bit input is supported";
    return false;
  }
  const int q = std::clamp(quality, 0, 100);
  const bool lossless = q == 0;

  Device dev;
  if (!dev.init(device_index, err)) {
    return false;
  }

  HandleGuardEncoder encoder;
  OIC_NVJPEG2K_OK(nvjpeg2kEncoderCreateSimple(&encoder.handle),
                  "nvjpeg2kEncoderCreateSimple");
  HandleGuardEncodeState state;
  OIC_NVJPEG2K_OK(nvjpeg2kEncodeStateCreate(encoder.handle, &state.handle),
                  "nvjpeg2kEncodeStateCreate");
  HandleGuardEncodeParams params;
  OIC_NVJPEG2K_OK(nvjpeg2kEncodeParamsCreate(&params.handle),
                  "nvjpeg2kEncodeParamsCreate");

  nvjpeg2kImageComponentInfo_t comps[3]{};
  for (uint32_t c = 0; c < static_cast<uint32_t>(img.channels); ++c) {
    comps[c].component_width = static_cast<uint32_t>(img.width);
    comps[c].component_height = static_cast<uint32_t>(img.height);
    comps[c].precision = 8;
    comps[c].sgn = 0;
  }

  nvjpeg2kEncodeConfig_t config{};
  config.stream_type = NVJPEG2K_STREAM_JP2;
  config.color_space =
      img.channels == 3 ? NVJPEG2K_COLORSPACE_SRGB : NVJPEG2K_COLORSPACE_GRAY;
  config.rsiz = 0;
  config.image_width = static_cast<uint32_t>(img.width);
  config.image_height = static_cast<uint32_t>(img.height);
  config.enable_tiling = 0;
  config.num_components = static_cast<uint32_t>(img.channels);
  config.image_comp_info = comps;
  config.prog_order = NVJPEG2K_LRCP;
  config.num_layers = 1;
  // RGB 走多分量变换（YCC/RCT），这也是 Q_FACTOR 质量因子被接受的前提。
  config.mct_mode = img.channels == 3 ? 1u : 0u;
  config.num_resolutions =
      resolution_levels(config.image_width, config.image_height);
  config.code_block_w = 64;
  config.code_block_h = 64;
  config.encode_modes = 0;
  config.irreversible = lossless ? 0u : 1u;

  OIC_NVJPEG2K_OK(nvjpeg2kEncodeParamsSetEncodeConfig(params.handle, &config),
                  "nvjpeg2kEncodeParamsSetEncodeConfig");
  if (!lossless) {
    OIC_NVJPEG2K_OK(nvjpeg2kEncodeParamsSpecifyQuality(
                        params.handle, NVJPEG2K_QUALITY_TYPE_Q_FACTOR, q),
                    "nvjpeg2kEncodeParamsSpecifyQuality");
  }
  OIC_NVJPEG2K_OK(nvjpeg2kEncodeParamsSetInputFormat(
                      params.handle, NVJPEG2K_FORMAT_INTERLEAVED),
                  "nvjpeg2kEncodeParamsSetInputFormat");

  // 设备侧是交错 8 bit 输入（1 或 3 通道）。
  const size_t row_bytes = img.stride();
  PitchBuffer buffer;
  buffer.budget = budget;
  if (!buffer.allocate(row_bytes, static_cast<uint32_t>(img.height), err)) {
    return false;
  }
  for (int y = 0; y < img.height; ++y) {
    if (!upload(dev.stream(),
                static_cast<uint8_t *>(buffer.ptr) +
                    static_cast<size_t>(y) * buffer.pitch,
                img.data.data() + static_cast<size_t>(y) * row_bytes, row_bytes,
                err)) {
      return false;
    }
  }

  nvjpeg2kImage_t src{};
  src.pixel_data = &buffer.ptr;
  src.pitch_in_bytes = &buffer.pitch;
  src.pixel_type = NVJPEG2K_UINT8;
  src.num_components = static_cast<uint32_t>(img.channels);

  OIC_NVJPEG2K_OK(nvjpeg2kEncode(encoder.handle, state.handle, params.handle,
                                 &src, dev.stream()),
                  "nvjpeg2kEncode");

  size_t len = 0;
  OIC_NVJPEG2K_OK(nvjpeg2kEncodeRetrieveBitstream(encoder.handle, state.handle,
                                                  nullptr, &len, dev.stream()),
                  "nvjpeg2kEncodeRetrieveBitstream(size)");
  if (len == 0) {
    if (err != nullptr) *err = "JPEG2000: nvJPEG2000 produced empty bitstream";
    return false;
  }
  out->resize(len);
  OIC_NVJPEG2K_OK(nvjpeg2kEncodeRetrieveBitstream(encoder.handle, state.handle,
                                                  out->data(), &len,
                                                  dev.stream()),
                  "nvjpeg2kEncodeRetrieveBitstream");
  if (!dev.synchronize(err)) {
    out->clear();
    return false;
  }
  out->resize(len);
  return true;
#endif
}

} // namespace jpeg2k
} // namespace nvidia
} // namespace image_codec
