/*
 * File: nvtiff_codec.cpp
 * Brief: TIFF codec on nvTIFF.
 * Author: Haomin Kong
 * Email: konghm@beim.com.cn / a645162@163.com
 * Created: 2026-11-03
 */

// nvtiff_codec.cpp - nvTIFF 编解码实现。
#include "nvtiff_codec.h"

#include <algorithm>
#include <cstring>

#include "nvidia_error.h"

#if defined(OIC_NVIDIA_HAS_NVTIFF)
#include <nvtiff.h>
#endif

namespace image_codec {
namespace nvidia {
namespace tiff {

namespace {

#if defined(OIC_NVIDIA_HAS_NVTIFF)

std::string status_text(nvtiffStatus_t st) {
  switch (st) {
  case NVTIFF_STATUS_SUCCESS:
    return "success";
  case NVTIFF_STATUS_NOT_INITIALIZED:
    return "nvTIFF not initialized";
  case NVTIFF_STATUS_INVALID_PARAMETER:
    return "invalid parameter";
  case NVTIFF_STATUS_BAD_TIFF:
    return "bad TIFF stream";
  case NVTIFF_STATUS_TIFF_NOT_SUPPORTED:
    return "TIFF not supported";
  case NVTIFF_STATUS_ALLOCATOR_FAILURE:
    return "allocator failure";
  case NVTIFF_STATUS_EXECUTION_FAILED:
    return "execution failed";
  case NVTIFF_STATUS_ARCH_MISMATCH:
    return "architecture mismatch";
  case NVTIFF_STATUS_INTERNAL_ERROR:
    return "internal error";
  case NVTIFF_STATUS_NVCOMP_NOT_FOUND:
    return "nvcomp library not found";
  case NVTIFF_STATUS_NVJPEG_NOT_FOUND:
    return "nvjpeg library not found";
  case NVTIFF_STATUS_TAG_NOT_FOUND:
    return "TIFF tag not found";
  case NVTIFF_STATUS_PARAMETER_OUT_OF_BOUNDS:
    return "parameter out of bounds";
  case NVTIFF_STATUS_NVJPEG2K_NOT_FOUND:
    return "nvJPEG2000 library not found";
  case NVTIFF_STATUS_BATCH_INCOMPATIBLE:
    return "regions cannot batch together";
  default:
    return "nvTIFF status " + std::to_string(static_cast<int>(st));
  }
}

#define OIC_NVTIFF_OK(call, what)                                              \
  do {                                                                         \
    const nvtiffStatus_t _st = (call);                                         \
    if (_st != NVTIFF_STATUS_SUCCESS) {                                        \
      if (err != nullptr) {                                                    \
        *err = std::string(what) + ": " + status_text(_st);                    \
      }                                                                        \
      return false;                                                            \
    }                                                                          \
  } while (0)

// pitch 分配 + 显存记账的 RAII。cudaMallocPitch 的行距天然满足 nvTIFF 对
// 「pitch 是样本字节数整数倍」的要求。
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

  bool allocate(size_t row_bytes, size_t height, std::string *err) {
    bytes = row_bytes * height;
    if (budget != nullptr && !budget->reserve(bytes, err)) {
      return false;
    }
    if (!check_cuda(cudaMallocPitch(&ptr, &pitch, row_bytes,
                                    static_cast<size_t>(height)),
                    "cudaMallocPitch", err)) {
      if (budget != nullptr) budget->release(bytes);
      bytes = 0;
      return false;
    }
    return true;
  }
};

bool compression_supported(int compression) {
  switch (compression) {
  case NVTIFF_COMPRESSION_NONE:
  case NVTIFF_COMPRESSION_LZW:
  case NVTIFF_COMPRESSION_JPEG:
  case NVTIFF_COMPRESSION_ADOBE_DEFLATE:
  case NVTIFF_COMPRESSION_DEFLATE:
  case NVTIFF_COMPRESSION_PACKBITS:
    return true;
  default:
    return false;
  }
}

#endif // OIC_NVIDIA_HAS_NVTIFF

} // namespace

bool available() {
#if defined(OIC_NVIDIA_HAS_NVTIFF)
  return true;
#else
  return false;
#endif
}

std::string version() {
#if defined(OIC_NVIDIA_HAS_NVTIFF)
  int major = 0;
  int minor = 0;
  if (nvtiffGetProperty(MAJOR_VERSION, &major) != NVTIFF_STATUS_SUCCESS ||
      nvtiffGetProperty(MINOR_VERSION, &minor) != NVTIFF_STATUS_SUCCESS) {
    return {};
  }
  return std::to_string(major) + "." + std::to_string(minor);
#else
  return {};
#endif
}

bool decode(int device_index, const uint8_t *data, size_t size,
            ImageBuffer *out, std::string *err, MemoryBudget *budget) {
#if !defined(OIC_NVIDIA_HAS_NVTIFF)
  (void)device_index;
  (void)data;
  (void)size;
  (void)out;
  (void)budget;
  if (err != nullptr) *err = "nvTIFF is not available in this build";
  return false;
#else
  if (data == nullptr || size == 0 || out == nullptr) {
    if (err != nullptr) *err = "TIFF: empty input";
    return false;
  }
  Device dev;
  if (!dev.init(device_index, err)) {
    return false;
  }

  nvtiffStream_t stream = nullptr;
  OIC_NVTIFF_OK(nvtiffStreamOpen(data, size, &stream), "nvtiffStreamOpen");
  struct StreamGuard {
    nvtiffStream_t stream = nullptr;
    ~StreamGuard() {
      if (stream != nullptr) (void)nvtiffStreamClose(stream);
    }
  } stream_guard{stream};

  nvtiffImageInfo_t info{};
  OIC_NVTIFF_OK(nvtiffStreamGetImageInfo(stream, 0, &info),
                "nvtiffStreamGetImageInfo");
  if (info.image_width == 0 || info.image_height == 0) {
    if (err != nullptr) *err = "TIFF: zero image dimension";
    return false;
  }
  // 通道保真：SamplesPerPixel 就是输出通道数，不做任何升维 / 降维。
  const int channels = static_cast<int>(info.samples_per_pixel);
  if (channels < 1 || channels > 4) {
    if (err != nullptr) {
      *err = "TIFF: " + std::to_string(channels) +
             " samples per pixel is not supported (1..4)";
    }
    return false;
  }
  int bits = 0;
  for (int c = 0; c < channels; ++c) {
    if (info.sample_format[c] != NVTIFF_SAMPLEFORMAT_UINT &&
        info.sample_format[c] != NVTIFF_SAMPLEFORMAT_UNKNOWN) {
      if (err != nullptr)
        *err = "TIFF: only unsigned integer samples are supported";
      return false;
    }
    if (c == 0) {
      bits = info.bits_per_sample[0];
    } else if (info.bits_per_sample[c] != bits) {
      if (err != nullptr)
        *err = "TIFF: samples with different bit depths are unsupported";
      return false;
    }
  }
  if (bits != 8 && bits != 16) {
    if (err != nullptr) {
      *err = "TIFF: unsupported sample bit depth " + std::to_string(bits);
    }
    return false;
  }
  const size_t bytes_per_sample = static_cast<size_t>(bits / 8);
  const size_t row_bytes = static_cast<size_t>(info.image_width) *
                           static_cast<size_t>(channels) * bytes_per_sample;

  nvtiffDecodeParams_t params = nullptr;
  OIC_NVTIFF_OK(nvtiffDecodeParamsCreate(&params), "nvtiffDecodeParamsCreate");
  struct ParamsGuard {
    nvtiffDecodeParams_t params = nullptr;
    ~ParamsGuard() {
      if (params != nullptr) (void)nvtiffDecodeParamsDestroy(params);
    }
  } params_guard{params};
  // UNCHANGED_I：保持文件里的原始样本格式与交错通道，通道数不会被强行变成 3。
  OIC_NVTIFF_OK(
      nvtiffDecodeParamsSetOutputFormat(params, NVTIFF_OUTPUT_UNCHANGED_I),
      "nvtiffDecodeParamsSetOutputFormat");

  nvtiffDecoder_t decoder = nullptr;
  OIC_NVTIFF_OK(nvtiffDecoderCreateSimple(&decoder, dev.stream()),
                "nvtiffDecoderCreateSimple");
  struct DecoderGuard {
    nvtiffDecoder_t decoder = nullptr;
    cudaStream_t stream = nullptr;
    ~DecoderGuard() {
      if (decoder != nullptr) (void)nvtiffDecoderDestroy(decoder, stream);
    }
  } decoder_guard{decoder, dev.stream()};

  PitchBuffer buffer;
  buffer.budget = budget;
  if (!buffer.allocate(row_bytes, info.image_height, err)) {
    return false;
  }

  void *planes[1] = {buffer.ptr};
  size_t pitches[1] = {buffer.pitch};
  nvtiffImage_t dst{};
  dst.plane_data = planes;
  dst.plane_pitch_bytes = pitches;
  dst.num_planes = 1;

  const nvtiffStatus_t st =
      nvtiffDecode(stream, decoder, params, &dst, dev.stream());
  // 无论成功与否都要等 stream 完成——decoder 复用了内部 scratch，提前销毁会
  // 在 GPU 还在跑的时候释放它。
  std::string sync_err;
  const bool synced = dev.synchronize(&sync_err);
  if (st != NVTIFF_STATUS_SUCCESS) {
    if (err != nullptr) *err = std::string("nvtiffDecode: ") + status_text(st);
    return false;
  }
  if (!synced) {
    if (err != nullptr) *err = sync_err;
    return false;
  }

  if (!out->allocate(static_cast<int>(info.image_width),
                     static_cast<int>(info.image_height), channels)) {
    if (err != nullptr) *err = "TIFF: allocation failed";
    return false;
  }

  const size_t out_row = out->stride();
  if (bits == 8) {
    for (uint32_t y = 0; y < info.image_height; ++y) {
      if (!download(dev.stream(),
                    out->data.data() + static_cast<size_t>(y) * out_row,
                    static_cast<const uint8_t *>(buffer.ptr) +
                        static_cast<size_t>(y) * buffer.pitch,
                    row_bytes, err)) {
        return false;
      }
    }
    return dev.synchronize(err);
  }

  // 16 bit：先整幅读回暂存，再统一右移到 8 bit（每行同步一次会白白多等 h 次）。
  std::vector<uint8_t> staging(buffer.bytes);
  if (!download(dev.stream(), staging.data(), buffer.ptr, buffer.bytes, err)) {
    return false;
  }
  if (!dev.synchronize(err)) {
    return false;
  }
  const auto *src16 = reinterpret_cast<const uint16_t *>(staging.data());
  const size_t samples = static_cast<size_t>(info.image_width) *
                         static_cast<size_t>(info.image_height) *
                         static_cast<size_t>(channels);
  for (size_t i = 0; i < samples; ++i) {
    out->data[i] = static_cast<uint8_t>(src16[i] >> 8);
  }
  return true;
#endif
}

bool encode(int device_index, const ImageBuffer &img,
            const EncodeOptions &options, std::vector<uint8_t> *out,
            std::string *err, MemoryBudget *budget) {
#if !defined(OIC_NVIDIA_HAS_NVTIFF)
  (void)device_index;
  (void)img;
  (void)options;
  (void)out;
  (void)budget;
  if (err != nullptr) *err = "nvTIFF is not available in this build";
  return false;
#else
  if (out == nullptr) {
    if (err != nullptr) *err = "TIFF: null output";
    return false;
  }
  out->clear();
  if (!img.valid() || (img.channels != 1 && img.channels != 3)) {
    if (err != nullptr)
      *err = "TIFF: only 1/3-channel 8-bit input is supported";
    return false;
  }
  if (!compression_supported(options.compression)) {
    if (err != nullptr) {
      *err = "TIFF: unsupported compression " +
             std::to_string(options.compression);
    }
    return false;
  }
  const size_t row_bytes = img.stride();
  const size_t height = static_cast<size_t>(img.height);
  int rows_per_strip = options.rows_per_strip;
  if (rows_per_strip <= 0) {
    rows_per_strip = static_cast<int>(std::min<size_t>(height, 256));
  }
  rows_per_strip = static_cast<int>(
      std::min<size_t>(static_cast<size_t>(rows_per_strip), height));
  const uint32_t num_striles =
      static_cast<uint32_t>((height + static_cast<size_t>(rows_per_strip) - 1) /
                            static_cast<size_t>(rows_per_strip));

  Device dev;
  if (!dev.init(device_index, err)) {
    return false;
  }

  // 输入在设备内存：nvTIFF 的编码入口只收设备指针（uint8_t**，没有 pitch 数组）。
  // 因此设备输入必须是**紧密打包**的行：用 cudaMallocPitch 会多出行尾 padding，
  // 而 nvTIFF 按 strile_width * bits_per_pixel 连续读，第 1 行起就会读到 padding，
  // 编出来的图在保持几何正确的前提下像素全错（自测里表现为「lossless 不相等」）。
  DeviceBuffer buffer;
  buffer.set_budget(budget);
  if (!buffer.ensure(row_bytes * height, err)) {
    return false;
  }
  for (int y = 0; y < img.height; ++y) {
    if (!upload(dev.stream(),
                static_cast<uint8_t *>(buffer.data()) +
                    static_cast<size_t>(y) * row_bytes,
                img.data.data() + static_cast<size_t>(y) * row_bytes, row_bytes,
                err)) {
      return false;
    }
  }

  nvtiffEncoder_t encoder = nullptr;
  OIC_NVTIFF_OK(nvtiffEncoderCreate(&encoder, nullptr, nullptr, dev.stream()),
                "nvtiffEncoderCreate");
  struct EncoderGuard {
    nvtiffEncoder_t encoder = nullptr;
    cudaStream_t stream = nullptr;
    ~EncoderGuard() {
      if (encoder != nullptr) (void)nvtiffEncoderDestroy(encoder, stream);
    }
  } encoder_guard{encoder, dev.stream()};

  nvtiffEncodeParams_t params = nullptr;
  OIC_NVTIFF_OK(nvtiffEncodeParamsCreate(&params), "nvtiffEncodeParamsCreate");
  struct ParamsGuard {
    nvtiffEncodeParams_t params = nullptr;
    cudaStream_t stream = nullptr;
    ~ParamsGuard() {
      if (params != nullptr) (void)nvtiffEncodeParamsDestroy(params, stream);
    }
  } params_guard{params, dev.stream()};

  nvtiffImageInfo_t info{};
  info.image_type = 0;
  info.image_width = static_cast<uint32_t>(img.width);
  info.image_height = static_cast<uint32_t>(img.height);
  info.compression = static_cast<nvtiffCompression_t>(options.compression);
  info.photometric_int = img.channels == 3 ? NVTIFF_PHOTOMETRIC_RGB
                                           : NVTIFF_PHOTOMETRIC_MINISBLACK;
  info.planar_config = NVTIFF_PLANARCONFIG_CONTIG;
  info.samples_per_pixel = static_cast<uint16_t>(img.channels);
  info.bits_per_pixel = static_cast<uint16_t>(8 * img.channels);
  for (int c = 0; c < img.channels; ++c) {
    info.bits_per_sample[c] = 8;
    info.sample_format[c] = NVTIFF_SAMPLEFORMAT_UINT;
  }
  OIC_NVTIFF_OK(nvtiffEncodeParamsSetImageInfo(params, &info),
                "nvtiffEncodeParamsSetImageInfo");

  nvtiffImageGeometry_t geometry{};
  geometry.type = NVTIFF_IMAGE_STRIPED;
  geometry.image_depth = 1;
  geometry.strile_width = static_cast<uint32_t>(img.width);
  geometry.strile_height = static_cast<uint32_t>(rows_per_strip);
  geometry.strile_depth = 1;
  geometry.num_striles_per_plane = num_striles;
  geometry.num_striles = num_striles;
  OIC_NVTIFF_OK(nvtiffEncodeParamsSetImageGeometry(params, &geometry),
                "nvtiffEncodeParamsSetImageGeometry");

  if (options.compression == NVTIFF_COMPRESSION_JPEG) {
    nvtiffJpegEncodeOptions_t jpeg{};
    jpeg.quality = std::clamp(options.jpeg_quality, 0, 100);
    jpeg.optimized_huffman = 0;
    jpeg.chroma_subsampling = NVTIFF_JPEG_CHROMA_SUBSAMPLING_DEFAULT;
    OIC_NVTIFF_OK(nvtiffEncodeParamsSetJpegOptions(params, &jpeg),
                  "nvtiffEncodeParamsSetJpegOptions");
  }

  auto *device_image = static_cast<uint8_t *>(buffer.data());
  OIC_NVTIFF_OK(nvtiffEncodeParamsSetInputs(params, &device_image, 1),
                "nvtiffEncodeParamsSetInputs");

  nvtiffEncodeParams_t param_array[1] = {params};
  OIC_NVTIFF_OK(nvtiffEncode(encoder, param_array, 1, dev.stream()),
                "nvtiffEncode");
  OIC_NVTIFF_OK(nvtiffEncodeFinalize(encoder, param_array, 1, dev.stream()),
                "nvtiffEncodeFinalize");

  size_t metadata_size = 0;
  size_t bitstream_size = 0;
  OIC_NVTIFF_OK(nvtiffGetBitstreamSize(encoder, param_array, 1, &metadata_size,
                                       &bitstream_size),
                "nvtiffGetBitstreamSize");
  const size_t total = metadata_size + bitstream_size;
  if (total == 0) {
    if (err != nullptr) *err = "TIFF: nvTIFF produced an empty bitstream";
    return false;
  }
  out->resize(total);
  OIC_NVTIFF_OK(nvtiffWriteTiffBuffer(encoder, param_array, 1, out->data(),
                                      total, dev.stream()),
                "nvtiffWriteTiffBuffer");
  if (!dev.synchronize(err)) {
    out->clear();
    return false;
  }
  out->resize(total);
  return true;
#endif
}

} // namespace tiff
} // namespace nvidia
} // namespace image_codec
