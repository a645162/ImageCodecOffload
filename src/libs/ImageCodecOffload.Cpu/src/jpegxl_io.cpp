#include "jpegxl_io.h"
#ifdef IMAGECODEC_OFFLOAD_WITH_JXL
#include <jxl/decode.h>
#include <jxl/encode.h>
#ifdef IMAGECODEC_OFFLOAD_WITH_JXL_THREADS
#include <jxl/thread_parallel_runner.h>
#endif
#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#endif

namespace image_codec::offload::cpu {
#ifdef IMAGECODEC_OFFLOAD_WITH_JXL
namespace {

// libjxl 默认使用单线程 runner；对于超大图，这会把默认 effort=7 的耗时放大到
// 不适合交互式批处理的程度。线程 runner 是 libjxl 的可选拆分库，所以构建没有
// 它时仍保持正确性，只是退回库的单线程实现。
class JxlParallelRunner final {
public:
  JxlParallelRunner() {
#ifdef IMAGECODEC_OFFLOAD_WITH_JXL_THREADS
    runner_ = JxlThreadParallelRunnerCreate(
        nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
#endif
  }

  ~JxlParallelRunner() {
#ifdef IMAGECODEC_OFFLOAD_WITH_JXL_THREADS
    if (runner_ != nullptr) JxlThreadParallelRunnerDestroy(runner_);
#endif
  }

  [[nodiscard]] bool setFor(JxlEncoder *encoder) const {
#ifdef IMAGECODEC_OFFLOAD_WITH_JXL_THREADS
    return runner_ == nullptr ||
           JxlEncoderSetParallelRunner(encoder, JxlThreadParallelRunner,
                                       runner_) == JXL_ENC_SUCCESS;
#else
    (void)encoder;
    return true;
#endif
  }

  [[nodiscard]] bool setFor(JxlDecoder *decoder) const {
#ifdef IMAGECODEC_OFFLOAD_WITH_JXL_THREADS
    return runner_ == nullptr ||
           JxlDecoderSetParallelRunner(decoder, JxlThreadParallelRunner,
                                       runner_) == JXL_DEC_SUCCESS;
#else
    (void)decoder;
    return true;
#endif
  }

private:
  void *runner_ = nullptr;
};

constexpr int kJxlDefaultEffort = 3;

struct ChunkedRowsContext final {
  int width = 0;
  int height = 0;
  const JpegXlRowReader *rowReader = nullptr;
  std::string error;
  Status errorCode = Status::EncodeFailed;
  std::mutex mutex;
};

void getChunkedPixelFormat(void *, JxlPixelFormat *format) {
  *format = JxlPixelFormat{3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
}

const void *getChunkedPixels(void *opaque, const size_t x, const size_t y,
                             const size_t width, const size_t height,
                             size_t *rowOffset) {
  auto *context = static_cast<ChunkedRowsContext *>(opaque);
  if (context == nullptr) return nullptr;
  const std::lock_guard lock(context->mutex);
  if (context->rowReader == nullptr || rowOffset == nullptr || width == 0 ||
      height == 0 || x > static_cast<size_t>(context->width) ||
      y > static_cast<size_t>(context->height) ||
      width > static_cast<size_t>(context->width) - x ||
      height > static_cast<size_t>(context->height) - y ||
      width > std::numeric_limits<size_t>::max() / 3 ||
      height > std::numeric_limits<size_t>::max() / (width * 3)) {
    context->error = "jxl: invalid chunk request";
    context->errorCode = Status::InvalidArgument;
    return nullptr;
  }

  try {
    const size_t chunkRowBytes = width * 3;
    auto pixels = std::make_unique<uint8_t[]>(chunkRowBytes * height);
    std::vector<uint8_t> fullRow(static_cast<size_t>(context->width) * 3);
    for (size_t row = 0; row < height; ++row) {
      auto result = (*context->rowReader)(static_cast<int>(y + row), fullRow);
      if (!result) {
        context->error = result.error().message;
        context->errorCode = result.error().code;
        return nullptr;
      }
      std::memcpy(pixels.get() + row * chunkRowBytes, fullRow.data() + x * 3,
                  chunkRowBytes);
    }
    *rowOffset = chunkRowBytes;
    return pixels.release();
  } catch (const std::bad_alloc &) {
    context->error = "jxl: chunk allocation failed";
    context->errorCode = Status::OutOfMemory;
  } catch (const std::exception &exception) {
    context->error =
        std::string("jxl: row callback failed: ") + exception.what();
    context->errorCode = Status::Internal;
  } catch (...) {
    context->error = "jxl: row callback failed with an unknown exception";
    context->errorCode = Status::Internal;
  }
  return nullptr;
}

void releaseChunkedPixels(void *, const void *pixels) {
  delete[] static_cast<const uint8_t *>(pixels);
}

} // namespace

Result<std::vector<uint8_t>> encodeJpegXl(const Image &image) {
  if (!image.valid() || image.channels < 1 || image.channels > 4)
    return fail<std::vector<uint8_t>>(Status::InvalidArgument,
                                      "jxl: invalid image");
  JxlEncoder *encoder = JxlEncoderCreate(nullptr);
  if (encoder == nullptr)
    return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                      "jxl: encoder allocation failed");
  JxlParallelRunner runner;
  if (!runner.setFor(encoder)) {
    JxlEncoderDestroy(encoder);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "jxl: parallel runner setup failed");
  }
  JxlBasicInfo info;
  JxlEncoderInitBasicInfo(&info);
  info.xsize = static_cast<uint32_t>(image.width);
  info.ysize = static_cast<uint32_t>(image.height);
  info.bits_per_sample = 8;
  info.num_color_channels = image.channels == 1 ? 1 : 3;
  info.num_extra_channels = image.channels == 4 ? 1 : 0;
  if (JxlEncoderSetBasicInfo(encoder, &info) != JXL_ENC_SUCCESS) {
    JxlEncoderDestroy(encoder);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "jxl: basic info failed");
  }
  JxlColorEncoding color;
  JxlColorEncodingSetToSRGB(&color, image.channels == 1);
  if (JxlEncoderSetColorEncoding(encoder, &color) != JXL_ENC_SUCCESS) {
    JxlEncoderDestroy(encoder);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "jxl: color encoding failed");
  }
  JxlEncoderFrameSettings *settings =
      JxlEncoderFrameSettingsCreate(encoder, nullptr);
  if (settings == nullptr ||
      JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT,
                                       kJxlDefaultEffort) != JXL_ENC_SUCCESS) {
    JxlEncoderDestroy(encoder);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "jxl: frame settings failed");
  }
  JxlPixelFormat format{static_cast<uint32_t>(image.channels), JXL_TYPE_UINT8,
                        JXL_NATIVE_ENDIAN, 0};
  if (JxlEncoderAddImageFrame(settings, &format, image.pixels.data(),
                              image.pixels.size()) != JXL_ENC_SUCCESS) {
    JxlEncoderDestroy(encoder);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "jxl: frame failed");
  }
  JxlEncoderCloseInput(encoder);
  std::vector<uint8_t> output;
  std::vector<uint8_t> chunk(1 << 20);
  uint8_t *next = chunk.data();
  size_t available = chunk.size();
  for (;;) {
    const auto status = JxlEncoderProcessOutput(encoder, &next, &available);
    output.insert(output.end(), chunk.data(), next);
    next = chunk.data();
    available = chunk.size();
    if (status == JXL_ENC_SUCCESS) break;
    if (status != JXL_ENC_NEED_MORE_OUTPUT) {
      JxlEncoderDestroy(encoder);
      return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                        "jxl: output failed");
    }
  }
  JxlEncoderDestroy(encoder);
  return output;
}

Result<std::vector<uint8_t>>
encodeJpegXlRows(const int width, const int height, const int channels,
                 const JpegXlRowReader &rowReader) {
  if (width <= 0 || height <= 0 || channels != 3 || !rowReader ||
      static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / 3) {
    return fail<std::vector<uint8_t>>(Status::InvalidArgument,
                                      "jxl: streaming encode requires positive "
                                      "RGB24 dimensions and a row reader");
  }
  JxlEncoder *encoder = JxlEncoderCreate(nullptr);
  if (encoder == nullptr)
    return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                      "jxl: encoder allocation failed");
  JxlParallelRunner runner;
  if (!runner.setFor(encoder)) {
    JxlEncoderDestroy(encoder);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "jxl: parallel runner setup failed");
  }
  JxlBasicInfo info;
  JxlEncoderInitBasicInfo(&info);
  info.xsize = static_cast<uint32_t>(width);
  info.ysize = static_cast<uint32_t>(height);
  info.bits_per_sample = 8;
  info.num_color_channels = 3;
  if (JxlEncoderSetBasicInfo(encoder, &info) != JXL_ENC_SUCCESS) {
    JxlEncoderDestroy(encoder);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "jxl: basic info failed");
  }
  JxlColorEncoding color;
  JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
  if (JxlEncoderSetColorEncoding(encoder, &color) != JXL_ENC_SUCCESS) {
    JxlEncoderDestroy(encoder);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "jxl: color encoding failed");
  }
  JxlEncoderFrameSettings *settings =
      JxlEncoderFrameSettingsCreate(encoder, nullptr);
  if (settings == nullptr ||
      JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT,
                                       kJxlDefaultEffort) != JXL_ENC_SUCCESS) {
    JxlEncoderDestroy(encoder);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "jxl: frame settings failed");
  }
  ChunkedRowsContext context{width, height, &rowReader, {}};
  JxlChunkedFrameInputSource input{};
  input.opaque = &context;
  input.get_color_channels_pixel_format = getChunkedPixelFormat;
  input.get_color_channel_data_at = getChunkedPixels;
  input.release_buffer = releaseChunkedPixels;
  if (JxlEncoderAddChunkedFrame(settings, JXL_TRUE, input) != JXL_ENC_SUCCESS) {
    JxlEncoderDestroy(encoder);
    return fail<std::vector<uint8_t>>(
        context.error.empty() ? Status::EncodeFailed : context.errorCode,
        context.error.empty() ? "jxl: chunked frame failed" : context.error);
  }
  JxlEncoderCloseInput(encoder);
  std::vector<uint8_t> output;
  std::vector<uint8_t> chunk(1 << 20);
  uint8_t *next = chunk.data();
  size_t available = chunk.size();
  for (;;) {
    const auto status = JxlEncoderProcessOutput(encoder, &next, &available);
    output.insert(output.end(), chunk.data(), next);
    next = chunk.data();
    available = chunk.size();
    if (status == JXL_ENC_SUCCESS) break;
    if (status != JXL_ENC_NEED_MORE_OUTPUT) {
      JxlEncoderDestroy(encoder);
      return fail<std::vector<uint8_t>>(
          context.error.empty() ? Status::EncodeFailed : context.errorCode,
          context.error.empty() ? "jxl: chunked output failed" : context.error);
    }
  }
  JxlEncoderDestroy(encoder);
  return output;
}

Result<Image> decodeJpegXl(const uint8_t *data, size_t size) {
  if (data == nullptr || size == 0)
    return fail<Image>(Status::InvalidArgument, "jxl: empty input");
  JxlDecoder *decoder = JxlDecoderCreate(nullptr);
  if (decoder == nullptr)
    return fail<Image>(Status::OutOfMemory, "jxl: decoder allocation failed");
  JxlParallelRunner runner;
  if (!runner.setFor(decoder)) {
    JxlDecoderDestroy(decoder);
    return fail<Image>(Status::DecodeFailed,
                       "jxl: parallel runner setup failed");
  }
  JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
  JxlDecoderSetInput(decoder, data, size);
  JxlBasicInfo info{};
  JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  Image image;
  for (;;) {
    const auto status = JxlDecoderProcessInput(decoder);
    if (status == JXL_DEC_BASIC_INFO) {
      if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS) break;
      format.num_channels = info.num_color_channels + info.num_extra_channels;
      if (format.num_channels < 1 || format.num_channels > 4) break;
      image.width = static_cast<int>(info.xsize);
      image.height = static_cast<int>(info.ysize);
      image.channels = static_cast<int>(format.num_channels);
      size_t outputSize = 0;
      if (JxlDecoderImageOutBufferSize(decoder, &format, &outputSize) !=
          JXL_DEC_SUCCESS)
        break;
      image.pixels.resize(outputSize);
      if (JxlDecoderSetImageOutBuffer(decoder, &format, image.pixels.data(),
                                      outputSize) != JXL_DEC_SUCCESS)
        break;
    } else if (status == JXL_DEC_FULL_IMAGE) {
      JxlDecoderDestroy(decoder);
      return image;
    } else if (status == JXL_DEC_SUCCESS) {
      break;
    } else if (status == JXL_DEC_NEED_MORE_INPUT) {
      break;
    } else {
      break;
    }
  }
  JxlDecoderDestroy(decoder);
  return fail<Image>(Status::DecodeFailed, "jxl: decode failed");
}
#else
Result<std::vector<uint8_t>> encodeJpegXl(const Image &) {
  return fail<std::vector<uint8_t>>(Status::BackendUnavailable,
                                    "jxl: libjxl is not built");
}
Result<std::vector<uint8_t>> encodeJpegXlRows(int, int, int,
                                              const JpegXlRowReader &) {
  return fail<std::vector<uint8_t>>(Status::BackendUnavailable,
                                    "jxl: libjxl is not built");
}
Result<Image> decodeJpegXl(const uint8_t *, size_t) {
  return fail<Image>(Status::BackendUnavailable, "jxl: libjxl is not built");
}
#endif
} // namespace image_codec::offload::cpu
