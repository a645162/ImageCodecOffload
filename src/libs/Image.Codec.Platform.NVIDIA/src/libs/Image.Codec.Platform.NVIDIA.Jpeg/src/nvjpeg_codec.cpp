/*
 * File: nvjpeg_codec.cpp
 * Brief: Baseline JPEG codec on nvJPEG.
 * Author: Haomin Kong
 * Email: konghm@beim.com.cn / a645162@163.com
 * Created: 2026-11-03
 */

// nvjpeg_codec.cpp - nvJPEG 编解码实现。
#include "nvjpeg_codec.h"

#include <algorithm>

#include "nvidia_error.h"

#if defined(OIC_NVIDIA_HAS_NVJPEG)
#include <nvjpeg.h>
#endif

namespace image_codec {
namespace nvidia {
namespace jpeg {

namespace {

#if defined(OIC_NVIDIA_HAS_NVJPEG)

// nvJPEG 状态码 -> 文本（用于错误上下文，不吞掉原始码）。
std::string nvjpeg_text(nvjpegStatus_t st) {
  switch (st) {
  case NVJPEG_STATUS_SUCCESS:
    return "success";
  case NVJPEG_STATUS_NOT_INITIALIZED:
    return "nvJPEG not initialized";
  case NVJPEG_STATUS_INVALID_PARAMETER:
    return "invalid parameter";
  case NVJPEG_STATUS_BAD_JPEG:
    return "bad JPEG bitstream";
  case NVJPEG_STATUS_JPEG_NOT_SUPPORTED:
    return "JPEG not supported (progressive/16-bit/...)";
  case NVJPEG_STATUS_ALLOCATOR_FAILURE:
    return "allocator failure";
  case NVJPEG_STATUS_EXECUTION_FAILED:
    return "execution failed";
  case NVJPEG_STATUS_ARCH_MISMATCH:
    return "architecture mismatch";
  case NVJPEG_STATUS_INTERNAL_ERROR:
    return "internal error";
  case NVJPEG_STATUS_IMPLEMENTATION_NOT_SUPPORTED:
    return "implementation not supported";
  default:
    return "nvJPEG status " + std::to_string(static_cast<int>(st));
  }
}

#define OIC_NVJPEG_OK(call, what)                                              \
  do {                                                                         \
    const nvjpegStatus_t _st = (call);                                         \
    if (_st != NVJPEG_STATUS_SUCCESS) {                                        \
      if (err != nullptr) {                                                    \
        *err = std::string(what) + ": " + nvjpeg_text(_st);                    \
      }                                                                        \
      return false;                                                            \
    }                                                                          \
  } while (0)

struct NvJpegHandle {
  nvjpegHandle_t handle = nullptr;
  ~NvJpegHandle() {
    if (handle != nullptr) (void)nvjpegDestroy(handle);
  }
};

// pitch 分配 + 显存记账的 RAII：nvJPEG 需要 pitch 对齐的行距，cudaMallocPitch
// 拿到的才是它要的；DeviceBuffer 只做整块 cudaMalloc，所以这里单独管。
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
};

nvjpegChromaSubsampling_t to_nvjpeg_css(int subsampling) {
  switch (subsampling) {
  case 444:
    return NVJPEG_CSS_444;
  case 422:
    return NVJPEG_CSS_422;
  case 420:
    return NVJPEG_CSS_420;
  default:
    return NVJPEG_CSS_420;
  }
}

#endif // OIC_NVIDIA_HAS_NVJPEG

} // namespace

bool available() {
#if defined(OIC_NVIDIA_HAS_NVJPEG)
  return true;
#else
  return false;
#endif
}

std::string version() {
#if defined(OIC_NVIDIA_HAS_NVJPEG)
  int major = 0;
  int minor = 0;
  if (nvjpegGetProperty(MAJOR_VERSION, &major) != NVJPEG_STATUS_SUCCESS ||
      nvjpegGetProperty(MINOR_VERSION, &minor) != NVJPEG_STATUS_SUCCESS) {
    return {};
  }
  return std::to_string(major) + "." + std::to_string(minor);
#else
  return {};
#endif
}

bool decode(int device_index, const uint8_t *data, size_t size,
            ImageBuffer *out, std::string *err, MemoryBudget *budget) {
#if !defined(OIC_NVIDIA_HAS_NVJPEG)
  (void)device_index;
  (void)data;
  (void)size;
  (void)out;
  (void)budget;
  if (err != nullptr) *err = "nvJPEG is not available in this build";
  return false;
#else
  if (data == nullptr || size == 0 || out == nullptr) {
    if (err != nullptr) *err = "JPEG: empty input";
    return false;
  }
  Device dev;
  if (!dev.init(device_index, err)) {
    return false;
  }
  NvJpegHandle nv;
  OIC_NVJPEG_OK(nvjpegCreateSimple(&nv.handle), "nvjpegCreateSimple");

  int ncomp = 0;
  nvjpegChromaSubsampling_t css = NVJPEG_CSS_UNKNOWN;
  int widths[NVJPEG_MAX_COMPONENT] = {0, 0, 0, 0};
  int heights[NVJPEG_MAX_COMPONENT] = {0, 0, 0, 0};
  OIC_NVJPEG_OK(
      nvjpegGetImageInfo(nv.handle, data, size, &ncomp, &css, widths, heights),
      "nvjpegGetImageInfo");
  if (widths[0] <= 0 || heights[0] <= 0) {
    if (err != nullptr) *err = "JPEG: zero image dimension";
    return false;
  }
  const int w = widths[0];
  const int h = heights[0];

  // 灰度 JPEG（单分量或 CSS_GRAY）必须按 1 通道输出。nvJPEG 的 RGBI 输出会把
  // 灰度展开成三份相同的值，于是同一张灰度图走 CPU 是 1 通道、走 GPU 变 3 通道。
  const bool gray = ncomp == 1 || css == NVJPEG_CSS_GRAY;
  const int out_channels = gray ? 1 : 3;
  const nvjpegOutputFormat_t out_format =
      gray ? NVJPEG_OUTPUT_Y : NVJPEG_OUTPUT_RGBI;
  const size_t row_bytes =
      static_cast<size_t>(w) * static_cast<size_t>(out_channels);

  nvjpegJpegState_t state = nullptr;
  OIC_NVJPEG_OK(nvjpegJpegStateCreate(nv.handle, &state),
                "nvjpegJpegStateCreate");
  struct StateGuard {
    nvjpegJpegState_t state = nullptr;
    ~StateGuard() {
      if (state != nullptr) (void)nvjpegJpegStateDestroy(state);
    }
  } state_guard{state};

  PitchBuffer buf;
  buf.budget = budget;
  buf.bytes = row_bytes * static_cast<size_t>(h);
  if (budget != nullptr && !budget->reserve(buf.bytes, err)) {
    return false;
  }
  if (!check_cuda(cudaMallocPitch(&buf.ptr, &buf.pitch, row_bytes, h),
                  "cudaMallocPitch", err)) {
    if (budget != nullptr) budget->release(buf.bytes);
    buf.bytes = 0;
    return false;
  }

  nvjpegImage_t dst{};
  dst.channel[0] = static_cast<unsigned char *>(buf.ptr);
  dst.pitch[0] = buf.pitch;

  const nvjpegStatus_t st = nvjpegDecode(nv.handle, state, data, size,
                                         out_format, &dst, dev.stream());
  if (st != NVJPEG_STATUS_SUCCESS) {
    if (err != nullptr) *err = std::string("nvjpegDecode: ") + nvjpeg_text(st);
    return false;
  }
  if (!dev.synchronize(err)) {
    return false;
  }

  if (!out->allocate(w, h, out_channels)) {
    if (err != nullptr) *err = "JPEG: allocation failed";
    return false;
  }
  // 逐行拷贝以吃掉 pitch padding。必须走 dev.stream()：那是非阻塞流，
  // 与 legacy default stream 之间没有隐式顺序，用 cudaMemcpy 会读到旧数据。
  for (int y = 0; y < h; ++y) {
    if (!check_cuda(cudaMemcpyAsync(
                        out->data.data() + static_cast<size_t>(y) * row_bytes,
                        static_cast<const uint8_t *>(buf.ptr) +
                            static_cast<size_t>(y) * buf.pitch,
                        row_bytes, cudaMemcpyDeviceToHost, dev.stream()),
                    "cudaMemcpyAsync D2H (jpeg rows)", err)) {
      return false;
    }
  }
  return dev.synchronize(err);
#endif
}

bool encode(int device_index, const ImageBuffer &img, int quality,
            int subsampling, std::vector<uint8_t> *out, std::string *err,
            MemoryBudget *budget) {
#if !defined(OIC_NVIDIA_HAS_NVJPEG)
  (void)device_index;
  (void)img;
  (void)quality;
  (void)subsampling;
  (void)out;
  (void)budget;
  if (err != nullptr) *err = "nvJPEG is not available in this build";
  return false;
#else
  if (out == nullptr) {
    if (err != nullptr) *err = "JPEG: null output";
    return false;
  }
  out->clear();
  if (!img.valid() || (img.channels != 3 && img.channels != 1)) {
    if (err != nullptr)
      *err = "JPEG: only 1/3-channel 8-bit input is supported";
    return false;
  }
  const int q = std::clamp(quality, 1, 100);
  const int css = subsampling == 0 ? (q >= 92 ? 444 : 420) : subsampling;

  // 灰度输入：nvJPEG 的编码输入只接受 3 通道（RGB/BGR/RGBI/BGRI），没有单通道
  // 枚举，因此宿主侧把灰度复制成 R=G=B 再喂进去；关键是采样因子必须设成
  // CSS_GRAY。只要求「输出 1 通道」而输入仍是 4:2:0，得到的就是一张三分量
  // JPEG，解回来必然 3 通道 —— 通道保真就没了。
  const bool gray = img.channels == 1;

  Device dev;
  if (!dev.init(device_index, err)) {
    return false;
  }
  NvJpegHandle nv;
  OIC_NVJPEG_OK(nvjpegCreateSimple(&nv.handle), "nvjpegCreateSimple");

  nvjpegEncoderState_t state = nullptr;
  nvjpegEncoderParams_t params = nullptr;
  OIC_NVJPEG_OK(nvjpegEncoderStateCreate(nv.handle, &state, dev.stream()),
                "nvjpegEncoderStateCreate");
  OIC_NVJPEG_OK(nvjpegEncoderParamsCreate(nv.handle, &params, dev.stream()),
                "nvjpegEncoderParamsCreate");
  struct EncoderGuard {
    nvjpegEncoderParams_t params = nullptr;
    nvjpegEncoderState_t state = nullptr;
    ~EncoderGuard() {
      if (params != nullptr) (void)nvjpegEncoderParamsDestroy(params);
      if (state != nullptr) (void)nvjpegEncoderStateDestroy(state);
    }
  } guard{params, state};

  OIC_NVJPEG_OK(nvjpegEncoderParamsSetQuality(params, q, dev.stream()),
                "nvjpegEncoderParamsSetQuality");
  OIC_NVJPEG_OK(
      nvjpegEncoderParamsSetSamplingFactors(
          params, gray ? NVJPEG_CSS_GRAY : to_nvjpeg_css(css), dev.stream()),
      "nvjpegEncoderParamsSetSamplingFactors");

  // 灰度：复制成 R=G=B 的交错 RGB。R、G、B 相等时亮度
  // Y = 0.299R + 0.587G + 0.114B 恰好等于 R，不引入额外色偏。
  std::vector<uint8_t> gray_staging;
  const uint8_t *src_plane = img.data.data();
  if (gray) {
    const size_t n = static_cast<size_t>(img.width) * img.height;
    gray_staging.resize(n * 3);
    for (size_t i = 0; i < n; ++i) {
      const uint8_t v = img.data[i];
      gray_staging[i * 3 + 0] = v;
      gray_staging[i * 3 + 1] = v;
      gray_staging[i * 3 + 2] = v;
    }
    src_plane = gray_staging.data();
  }

  // 设备侧始终是交错 RGB（nvJPEG 编码输入的唯一可用形态）。
  const size_t row_bytes = static_cast<size_t>(img.width) * 3;

  PitchBuffer buf;
  buf.budget = budget;
  buf.bytes = row_bytes * static_cast<size_t>(img.height);
  if (budget != nullptr && !budget->reserve(buf.bytes, err)) {
    return false;
  }
  if (!check_cuda(cudaMallocPitch(&buf.ptr, &buf.pitch, row_bytes, img.height),
                  "cudaMallocPitch", err)) {
    if (budget != nullptr) budget->release(buf.bytes);
    buf.bytes = 0;
    return false;
  }
  for (int y = 0; y < img.height; ++y) {
    if (!check_cuda(
            cudaMemcpyAsync(static_cast<uint8_t *>(buf.ptr) +
                                static_cast<size_t>(y) * buf.pitch,
                            src_plane + static_cast<size_t>(y) * row_bytes,
                            row_bytes, cudaMemcpyHostToDevice, dev.stream()),
            "cudaMemcpyAsync H2D (jpeg rows)", err)) {
      return false;
    }
  }

  nvjpegImage_t src{};
  src.channel[0] = static_cast<unsigned char *>(buf.ptr);
  src.pitch[0] = buf.pitch;
  const nvjpegStatus_t est =
      nvjpegEncodeImage(nv.handle, state, params, &src, NVJPEG_INPUT_RGBI,
                        img.width, img.height, dev.stream());
  if (est != NVJPEG_STATUS_SUCCESS) {
    if (err != nullptr)
      *err = std::string("nvjpegEncodeImage: ") + nvjpeg_text(est);
    return false;
  }

  size_t len = 0;
  OIC_NVJPEG_OK(nvjpegEncodeRetrieveBitstream(nv.handle, state, nullptr, &len,
                                              dev.stream()),
                "nvjpegEncodeRetrieveBitstream(size)");
  if (len == 0) {
    if (err != nullptr) *err = "JPEG: nvJPEG produced empty bitstream";
    return false;
  }
  out->resize(len);
  const nvjpegStatus_t rst = nvjpegEncodeRetrieveBitstream(
      nv.handle, state, out->data(), &len, dev.stream());
  if (rst != NVJPEG_STATUS_SUCCESS) {
    if (err != nullptr) {
      *err = std::string("nvjpegEncodeRetrieveBitstream: ") + nvjpeg_text(rst);
    }
    out->clear();
    return false;
  }
  out->resize(len);
  return true;
#endif
}

} // namespace jpeg
} // namespace nvidia
} // namespace image_codec
