/*
 * File: ico_test.cpp
 * Brief: Ico test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// ico_test.cpp - 路由层的自检（返回码约定：0 = PASS、2 = SKIP、其它 = FAIL）。
//
// 检查的不是「能跑通」，而是四件最容易悄悄错的事：
//   1. BMP 中转格式自己先要对（否则后面所有对比都在比错的像素）；
//   2. CPU 基线的往返：无损格式必须逐位一致，JPEG 必须达到合理 PSNR；
//   3. 指标与另外两个子项目给出**同一个数字**（golden 值）；
//   4. 每个可用后端的指标与 CPU 参考逐项对拍；回退路径必须留下具体原因。
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <ImageCodecOffload/image_codec_offload.h>

#include "bmp_io.h"
#include "image_codec_types.h"
#include "metrics.h"
#include "openjpeg_io.h"
#include "router.h"

using image_codec::offload::BackendKind;
using image_codec::offload::EncodeCapabilities;
using image_codec::offload::EncodeParams;
using image_codec::offload::ExecutionMode;
using image_codec::offload::Format;
using image_codec::offload::Image;
using image_codec::offload::Jpeg2000Container;
using image_codec::offload::Router;
using image_codec::offload::SsimResult;
using image_codec::offload::Status;

namespace {

int g_failures = 0;

struct StreamingRowReaderState {
  const Image *image = nullptr;
  int rowsRead = 0;
  int failAtRow = -1;
};

ico_status readStreamingRow(void *user, const int y, uint8_t *destination,
                            const size_t destinationSize) {
  auto *state = static_cast<StreamingRowReaderState *>(user);
  if (state == nullptr || state->image == nullptr || destination == nullptr ||
      y < 0 || y >= state->image->height ||
      destinationSize != state->image->rowBytes()) {
    return ICO_ERR_INVALID_ARGUMENT;
  }
  if (y == state->failAtRow) return ICO_ERR_IO;
  std::copy_n(state->image->pixels.data() +
                  static_cast<size_t>(y) * state->image->rowBytes(),
              state->image->rowBytes(), destination);
  ++state->rowsRead;
  return ICO_OK;
}

void check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

Image makeImage(int w, int h, int c);

void testFormatCatalogAndProbe() {
  const auto &descriptors = image_codec::offload::formatDescriptors();
  check(descriptors.size() == 5, "format catalog has five formats");
  const auto *jpeg2000 = image_codec::offload::formatDescriptor(Format::Jpeg2000);
  check(jpeg2000 != nullptr, "jpeg2000 descriptor");
  if (jpeg2000 != nullptr) {
    check(jpeg2000->theoreticalMaxWidth ==
              std::numeric_limits<uint32_t>::max() &&
              jpeg2000->theoreticalMaxHeight ==
                  std::numeric_limits<uint32_t>::max(),
          "jpeg2000 exposes 32-bit container dimensions");
    check(jpeg2000->implementationMaxWidth ==
              static_cast<uint64_t>(std::numeric_limits<int>::max()) &&
              jpeg2000->implementationMaxHeight ==
                  static_cast<uint64_t>(std::numeric_limits<int>::max()),
          "jpeg2000 is bounded by the public image model");
  }
  const auto *jpegXl = image_codec::offload::formatDescriptor(Format::JpegXl);
  check(jpegXl != nullptr, "jpeg-xl descriptor");
  if (jpegXl != nullptr) {
    check(jpegXl->theoreticalMaxWidth == std::numeric_limits<uint32_t>::max() &&
              jpegXl->theoreticalMaxHeight == std::numeric_limits<uint32_t>::max(),
          "jpeg-xl exposes 32-bit container dimensions");
    check(jpegXl->implementationMaxWidth ==
              static_cast<uint64_t>(std::numeric_limits<int>::max()) &&
              jpegXl->implementationMaxHeight ==
                  static_cast<uint64_t>(std::numeric_limits<int>::max()),
          "jpeg-xl is bounded by the public image model");
  }
  check(image_codec::offload::formatFromExtension("JP2") == Format::Jpeg2000,
        "jp2 extension");
  check(image_codec::offload::formatFromExtension("jpf") == Format::Jpeg2000,
        "jpf extension");
  check(image_codec::offload::formatFromExtension("jpx") == Format::Jpeg2000,
        "jpx extension");
  check(image_codec::offload::formatFromExtension("jxl") == Format::JpegXl,
        "jxl extension");
  const uint8_t jp2[] = {0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ', 0x0D,
                         0x0A, 0x87, 0x0A};
  check(image_codec::offload::probeFormat(jp2, sizeof(jp2)) == Format::Jpeg2000,
        "jp2 signature");
  const uint8_t jxl[] = {0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D,
                         0x0A, 0x87, 0x0A};
  check(image_codec::offload::probeFormat(jxl, sizeof(jxl)) == Format::JpegXl,
        "jxl signature");

  int c_format = ICO_FORMAT_UNKNOWN;
  check(image_codec_offload_probe(jp2, sizeof(jp2), &c_format) == ICO_OK &&
            c_format == ICO_FORMAT_JPEG2000,
        "c api jp2 signature");
  check(image_codec_offload_probe(jxl, sizeof(jxl), &c_format) == ICO_OK &&
            c_format == ICO_FORMAT_JPEG_XL,
        "c api jxl signature");
}

void testExecutionModeContract() {
  Router router;
  check(router.executionMode(BackendKind::Cpu, Format::Jpeg) ==
            ExecutionMode::Cpu,
        "cpu jpeg reports cpu execution mode");
#ifdef IMAGECODEC_OFFLOAD_WITH_JXL
  check(router.executionMode(BackendKind::Cpu, Format::JpegXl) ==
            ExecutionMode::Cpu,
        "cpu jpeg-xl execution mode follows build capability");
#else
  check(router.executionMode(BackendKind::Cpu, Format::JpegXl) ==
            ExecutionMode::Unavailable,
        "cpu jpeg-xl execution mode follows build capability");
#endif
  check(std::string(image_codec::offload::executionModeName(
            ExecutionMode::HybridCpuGpu)) == "hybrid-cpu-gpu",
        "execution mode has stable display name");
  for (const BackendKind kind : {BackendKind::OpenCl, BackendKind::Cuda,
                                 BackendKind::Vulkan}) {
    if (!router.supports(kind, Format::Jpeg)) {
      check(router.executionMode(kind, Format::Jpeg) ==
                ExecutionMode::Unavailable,
            "unsupported gpu format reports unavailable mode");
    } else {
      check(router.executionMode(kind, Format::Jpeg) ==
                ExecutionMode::HybridCpuGpu,
            "gpu jpeg reports hybrid execution mode");
    }
  }
}

void testRuntimeResolutionLimitContract() {
  Router router;
  for (const BackendKind backend : {BackendKind::Cpu, BackendKind::OpenCl,
                                    BackendKind::Cuda, BackendKind::Vulkan}) {
    const auto limit = router.runtimeResolutionLimit(backend, Format::Jpeg);
    if (!router.supports(backend, Format::Jpeg)) {
      check(!limit && (limit.error().code == Status::BackendUnavailable ||
                       limit.error().code == Status::UnsupportedFormat),
            "unbuilt backend reports an explicit dynamic-limit error");
      continue;
    }
    if (!limit) {
      check(limit.error().code == Status::BackendUnavailable,
            "unavailable backend reports backend-unavailable dynamic limit");
      continue;
    }
    check(!limit->detail.empty(),
          "runtime resolution limit always states its calculation basis");
    if (limit->maxPixels != 0) {
      check(limit->bytesPerPixel != 0,
            "numeric runtime limit states bytes per pixel");
      check(limit->availableBytes / limit->bytesPerPixel == limit->maxPixels,
            "runtime pixels derive exactly from the reported budget");
    }
  }
}

void testJpeg2000CpuRoundtrip() {
  Router router;
  const Image source = makeImage(37, 23, 3);
  EncodeParams params;
  auto encoded = router.encodeWith(BackendKind::Cpu, Format::Jpeg2000,
                                   source, params);
  if (!encoded) {
    check(encoded.error().code == Status::BackendUnavailable,
          "jpeg2000 is either available or explicitly unavailable");
    return;
  }
  check(!encoded->empty(), "jpeg2000 cpu encode");
  auto decoded = router.decodeWith(BackendKind::Cpu, Format::Jpeg2000,
                                   encoded->data(), encoded->size());
  check(static_cast<bool>(decoded), "jpeg2000 cpu decode");
  if (decoded) {
    check(decoded->width == source.width && decoded->height == source.height,
          "jpeg2000 geometry");
    check(decoded->channels == source.channels, "jpeg2000 channels");
  }

  // .jp2/.jpf/.jpx 都使用 JP2 家族容器。矩阵将以三个文件后缀分别落盘，
  // 这里锁住它们的共同码流语义，避免后续只测了扩展名却改坏实际容器。
  for (const char *label : {"jp2", "jpf", "jpx"}) {
    const auto jp2_family = router.encodeWith(BackendKind::Cpu,
                                              Format::Jpeg2000, source,
                                              EncodeParams{});
    check(static_cast<bool>(jp2_family), "jp2-family cpu encode");
    if (!jp2_family) continue;
    check(jp2_family->size() >= 12 && (*jp2_family)[4] == 'j' &&
              (*jp2_family)[5] == 'P' && (*jp2_family)[6] == ' ' &&
              (*jp2_family)[7] == ' ',
          label);
    const auto decoded_family =
        router.decodeWith(BackendKind::Cpu, Format::Jpeg2000,
                          jp2_family->data(), jp2_family->size());
    check(static_cast<bool>(decoded_family), "jp2-family cpu decode");
  }

  EncodeParams j2k_params;
  j2k_params.jpeg2000_container = Jpeg2000Container::J2k;
  auto j2k = router.encodeWith(BackendKind::Cpu, Format::Jpeg2000, source,
                               j2k_params);
  check(static_cast<bool>(j2k), "j2k codestream cpu encode");
  if (j2k) {
    check(j2k->size() >= 4 && (*j2k)[0] == 0xff && (*j2k)[1] == 0x4f &&
              (*j2k)[2] == 0xff && (*j2k)[3] == 0x51,
          "j2k output has codestream marker");
    auto decoded_j2k = router.decodeWith(BackendKind::Cpu, Format::Jpeg2000,
                                         j2k->data(), j2k->size());
    check(static_cast<bool>(decoded_j2k), "j2k codestream cpu decode");
    if (decoded_j2k) {
      check(decoded_j2k->width == source.width &&
                decoded_j2k->height == source.height &&
                decoded_j2k->channels == source.channels,
            "j2k codestream geometry");
    }
  }
}

void testJpeg2000RowStreaming() {
  constexpr int kWidth = 41;
  constexpr int kHeight = 301;  // Crosses the 256-row tile boundary.
  constexpr int kChannels = 3;
  const Image source = makeImage(kWidth, kHeight, kChannels);
  const auto path = std::filesystem::temp_directory_path() /
                    "imagecodec-jpeg2000-row-streaming.jp2";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  int rowsRead = 0;
  const auto streamed = image_codec::offload::cpu::encodeJpeg2000Rows(
      path, kWidth, kHeight, kChannels, Jpeg2000Container::Jp2,
      [&source, &rowsRead](const int y, std::span<uint8_t> destination)
          -> image_codec::offload::Result<void> {
        if (y < 0 || y >= source.height ||
            destination.size() != source.rowBytes()) {
          return image_codec::offload::fail<void>(
              Status::InvalidArgument, "test row callback received bad range");
        }
        std::copy_n(source.pixels.data() +
                        static_cast<size_t>(y) * source.rowBytes(),
                    source.rowBytes(), destination.data());
        ++rowsRead;
        return {};
      });
  if (!streamed) {
    check(streamed.error().code == Status::BackendUnavailable,
          "jpeg2000 streaming is available when OpenJPEG is built");
    std::filesystem::remove(path, ignored);
    return;
  }
  check(rowsRead == kHeight, "jpeg2000 streaming reads each row exactly once");

  std::ifstream input(path, std::ios::binary);
  const std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
  std::filesystem::remove(path, ignored);
  check(!encoded.empty(), "jpeg2000 streaming produces a JP2 file");
  Router router;
  const auto decoded = router.decodeWith(BackendKind::Cpu, Format::Jpeg2000,
                                         encoded.data(), encoded.size());
  check(static_cast<bool>(decoded), "jpeg2000 streaming output decodes");
  if (decoded) {
    check(decoded->width == kWidth && decoded->height == kHeight &&
              decoded->channels == kChannels,
          "jpeg2000 streaming preserves geometry and channels");
  }
}

void testJpeg2000RowStreamingCapi() {
  constexpr int kWidth = 37;
  constexpr int kHeight = 263;  // Crosses the tile boundary.
  constexpr int kChannels = 3;
  const Image source = makeImage(kWidth, kHeight, kChannels);
  const auto path = std::filesystem::temp_directory_path() /
                    "imagecodec-jpeg2000-row-streaming.j2k";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  ico_codec *codec = nullptr;
  check(image_codec_offload_create_forced(&codec, ICO_BACKEND_CPU) == ICO_OK &&
            codec != nullptr,
        "create forced CPU codec for jpeg2000 streaming");
  if (codec == nullptr) return;
  StreamingRowReaderState state{.image = &source};
  const auto status = image_codec_offload_encode_jpeg2000_rows(
      codec, path.string().c_str(), kWidth, kHeight, kChannels,
      ICO_JPEG2000_CONTAINER_J2K, readStreamingRow, &state);
  if (status != ICO_OK) {
    check(status == ICO_ERR_BACKEND_UNAVAILABLE,
          "jpeg2000 streaming C API is available when OpenJPEG is built");
    image_codec_offload_destroy(codec);
    std::filesystem::remove(path, ignored);
    return;
  }
  check(state.rowsRead == kHeight,
        "jpeg2000 streaming C API reads each row exactly once");
  int mode = ICO_EXECUTION_UNAVAILABLE;
  check(image_codec_offload_last_execution_mode(codec, &mode) == ICO_OK &&
            mode == ICO_EXECUTION_CPU,
        "jpeg2000 streaming C API truthfully reports CPU mode");
  image_codec_offload_destroy(codec);

  std::ifstream input(path, std::ios::binary);
  const std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
  std::filesystem::remove(path, ignored);
  check(encoded.size() >= 4 && encoded[0] == 0xFF && encoded[1] == 0x4F &&
            encoded[2] == 0xFF && encoded[3] == 0x51,
        "jpeg2000 streaming C API writes the J2K codestream");
  Router router;
  const auto decoded = router.decodeWith(BackendKind::Cpu, Format::Jpeg2000,
                                         encoded.data(), encoded.size());
  check(static_cast<bool>(decoded), "jpeg2000 streaming C API output decodes");
  if (decoded) {
    check(decoded->width == kWidth && decoded->height == kHeight &&
              decoded->channels == kChannels,
          "jpeg2000 streaming C API preserves geometry and channels");
  }
}

void testJpeg2000RowStreamingCapiPropagatesReaderError() {
  const Image source = makeImage(17, 19, 3);
  const auto path = std::filesystem::temp_directory_path() /
                    "imagecodec-jpeg2000-row-streaming-failure.jp2";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  ico_codec *codec = nullptr;
  check(image_codec_offload_create_forced(&codec, ICO_BACKEND_CPU) == ICO_OK &&
            codec != nullptr,
        "create forced CPU codec for streaming reader error");
  if (codec == nullptr) return;
  StreamingRowReaderState state{.image = &source, .failAtRow = 3};
  const auto status = image_codec_offload_encode_jpeg2000_rows(
      codec, path.string().c_str(), source.width, source.height,
      source.channels, ICO_JPEG2000_CONTAINER_JP2, readStreamingRow, &state);
  image_codec_offload_destroy(codec);
  if (status == ICO_ERR_BACKEND_UNAVAILABLE) {
    std::filesystem::remove(path, ignored);
    return;
  }
  check(status == ICO_ERR_IO,
        "jpeg2000 streaming C API preserves row-reader error status");
  check(state.rowsRead == 3,
        "jpeg2000 streaming stops before consuming rows after reader error");
  check(!std::filesystem::exists(path),
        "jpeg2000 streaming removes partial output after reader error");
}

void testJpegXlCpuRoundtrip() {
  Router router;
  const Image source = makeImage(31, 19, 4);
  auto encoded = router.encodeWith(BackendKind::Cpu, Format::JpegXl, source,
                                   EncodeParams{});
  if (!encoded) {
    check(encoded.error().code == Status::BackendUnavailable,
          "jpeg-xl is either available or explicitly unavailable");
    return;
  }
  auto decoded = router.decodeWith(BackendKind::Cpu, Format::JpegXl,
                                   encoded->data(), encoded->size());
  check(static_cast<bool>(decoded), "jpeg-xl cpu decode");
  if (decoded) {
    check(decoded->width == source.width && decoded->height == source.height,
          "jpeg-xl geometry");
    check(decoded->channels == source.channels, "jpeg-xl channels");
  }
}

void testGpuHybridNewFormatRoundtrip() {
  Router router;
  const Image source = makeImage(29, 17, 3);
  for (const BackendKind backend : {BackendKind::OpenCl, BackendKind::Cuda,
                                    BackendKind::Vulkan}) {
    for (const Format format : {Format::Jpeg2000, Format::JpegXl}) {
      if (!router.supports(backend, format)) {
        check(router.executionMode(backend, format) ==
                  ExecutionMode::Unavailable,
              "unbuilt gpu new format reports unavailable");
        continue;
      }
      check(router.executionMode(backend, format) ==
                ExecutionMode::HybridCpuGpu,
            "gpu new format reports hybrid execution mode");
      const auto encoded =
          router.encodeWith(backend, format, source, EncodeParams{});
      if (!encoded) {
        check(encoded.error().code == Status::BackendUnavailable,
              "gpu new format failure is an explicit availability error");
        continue;
      }
      check(!encoded->empty(), "gpu new format produced bytes");
      const auto decoded =
          router.decodeWith(backend, format, encoded->data(), encoded->size());
      check(static_cast<bool>(decoded), "gpu new format decoded output");
      if (decoded) {
        check(decoded->width == source.width && decoded->height == source.height,
              "gpu new format geometry");
        check(decoded->channels == source.channels, "gpu new format channels");
      }
    }
  }
}

// 确定性图案（纯渐变会让压缩缺陷被掩盖；这里要的是有局部随机性）。
// 与 ImageCodecCUDA / ImageCodecOpenCL 的同名测试逐字相同：三个后端必须在
// 同一张图上给出同一组数字。
Image makeImage(int w, int h, int c) {
  Image img;
  img.width = w;
  img.height = h;
  img.channels = c;
  img.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) *
                    static_cast<size_t>(c));

  std::mt19937 rng(12345);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (int ch = 0; ch < c; ++ch) {
        const size_t idx =
            (static_cast<size_t>(y) * w + x) * static_cast<size_t>(c) + ch;
        const int v =
            (x * 3 + y * 5 + ch * 37 + static_cast<int>(rng() % 7)) & 0xFF;
        img.pixels[idx] = static_cast<uint8_t>(v);
      }
    }
  }
  return img;
}

Image perturb(const Image &src, unsigned seed) {
  Image out = src;
  std::mt19937 rng(seed);
  for (auto &v : out.pixels) {
    const int d = static_cast<int>(rng() % 5) - 2;
    const int nv = static_cast<int>(v) + d;
    v = static_cast<uint8_t>(nv < 0 ? 0 : (nv > 255 ? 255 : nv));
  }
  return out;
}

// ---- 1. BMP 中转 ----
//
// BMP 在本项目里只是调试/CLI 的中转格式，但它一旦丢掉 Alpha，后面所有比对
// 就都在比错的数据。因此 1/3/4 通道都要逐位往返；2 通道（灰度+Alpha）在 BMP
// 里没有表示法，必须**明确拒绝**而不是伪造一个 32-bit 文件。
void testBmpRoundtrip() {
  for (int channels : {1, 3, 4}) {
    const Image src = makeImage(37, 23, channels);

    auto encoded = image_codec::offload::common::writeBmp(src);
    check(static_cast<bool>(encoded), "bmp write");

    auto decoded =
        image_codec::offload::common::readBmp(encoded->data(), encoded->size());
    check(static_cast<bool>(decoded), "bmp read");
    if (!decoded) continue;

    check(decoded->width == src.width && decoded->height == src.height,
          "bmp geometry");
    check(decoded->channels == src.channels, "bmp channel fidelity");
    check(decoded->pixels == src.pixels, "bmp pixels identical");
  }

  const Image ga = makeImage(8, 8, 2);
  check(!image_codec::offload::common::writeBmp(ga),
        "bmp rejects 2-channel (gray+alpha) instead of faking a 32-bit file");
}

// ---- 2. CPU 基线往返 ----

void testCpuRoundtrip() {
  Router router;
  const Image src = makeImage(257, 129, 3); // 故意不是 16 的倍数

  // TIFF 无压缩：无损，必须逐位一致。
  {
    EncodeParams params;
    params.tiff_compression = 1;
    auto encoded =
        router.encodeWith(BackendKind::Cpu, Format::Tiff, src, params);
    check(static_cast<bool>(encoded), "tiff(none) encode");
    if (encoded) {
      auto decoded = router.decodeWith(BackendKind::Cpu, Format::Tiff,
                                       encoded->data(), encoded->size());
      check(static_cast<bool>(decoded), "tiff(none) decode");
      if (decoded) check(decoded->pixels == src.pixels, "tiff(none) lossless");
    }
  }

  // TIFF LZW：同样无损，但走的是 libtiff 的压缩路径。
  {
    EncodeParams params;
    params.tiff_compression = 5;
    auto encoded =
        router.encodeWith(BackendKind::Cpu, Format::Tiff, src, params);
    check(static_cast<bool>(encoded), "tiff(lzw) encode");
    if (encoded) {
      auto decoded = router.decodeWith(BackendKind::Cpu, Format::Tiff,
                                       encoded->data(), encoded->size());
      check(static_cast<bool>(decoded), "tiff(lzw) decode");
      if (decoded) check(decoded->pixels == src.pixels, "tiff(lzw) lossless");
    }
  }

  // TIFF tiled：边缘块要用边缘像素填充，尺寸不能被截断到 tile 的整数倍。
  {
    EncodeParams params;
    params.tiff_compression = 1;
    params.tiff_tile_size = 64;
    auto encoded =
        router.encodeWith(BackendKind::Cpu, Format::Tiff, src, params);
    check(static_cast<bool>(encoded), "tiff(tiled) encode");
    if (encoded) {
      auto decoded = router.decodeWith(BackendKind::Cpu, Format::Tiff,
                                       encoded->data(), encoded->size());
      check(static_cast<bool>(decoded), "tiff(tiled) decode");
      if (decoded) {
        check(decoded->width == src.width && decoded->height == src.height,
              "tiff(tiled) keeps odd geometry");
        check(decoded->pixels == src.pixels, "tiff(tiled) lossless");
      }
    }
  }

  // PNG：无损。
  {
    EncodeParams params;
    auto encoded =
        router.encodeWith(BackendKind::Cpu, Format::Png, src, params);
    check(static_cast<bool>(encoded), "png encode");
    if (encoded) {
      auto decoded = router.decodeWith(BackendKind::Cpu, Format::Png,
                                       encoded->data(), encoded->size());
      check(static_cast<bool>(decoded), "png decode");
      if (decoded) check(decoded->pixels == src.pixels, "png lossless");
    }
  }

  // 灰度通道保真：1 通道进、1 通道出，绝不升维成 3 通道。
  {
    const Image gray = makeImage(64, 48, 1);
    EncodeParams params;
    for (Format format : {Format::Png, Format::Tiff, Format::Jpeg}) {
      auto encoded = router.encodeWith(BackendKind::Cpu, format, gray, params);
      check(static_cast<bool>(encoded), "gray encode");
      if (!encoded) continue;
      auto decoded = router.decodeWith(BackendKind::Cpu, format,
                                       encoded->data(), encoded->size());
      check(static_cast<bool>(decoded), "gray decode");
      if (decoded) {
        check(decoded->channels == 1, "gray stays 1 channel");
      }
    }
  }

  // JPEG：有损，用 PSNR 兜住质量下限。
  //
  // 这里刻意关掉色度子采样（4:4:4）：本图是逐像素随机的合成图案，4:2:0 会把
  // 色度抹平，PSNR 掉到 23 dB —— 那测的是子采样的性质，不是编码器的正确性。
  {
    EncodeParams params;
    params.jpeg_quality = 92;
    params.jpeg_subsampling = 444;
    auto encoded =
        router.encodeWith(BackendKind::Cpu, Format::Jpeg, src, params);
    check(static_cast<bool>(encoded), "jpeg encode");
    if (encoded) {
      auto decoded = router.decodeWith(BackendKind::Cpu, Format::Jpeg,
                                       encoded->data(), encoded->size());
      check(static_cast<bool>(decoded), "jpeg decode");
      if (decoded) {
        check(decoded->width == src.width && decoded->height == src.height,
              "jpeg geometry");
        const auto psnr = image_codec::offload::metrics::psnr(src, *decoded);
        check(static_cast<bool>(psnr), "jpeg psnr");
        if (psnr) check(*psnr > 30.0, "jpeg quality >= 30 dB at q=92");
        if (psnr) {
          std::printf("jpeg q=92: %.2f dB, %zu bytes\n", *psnr,
                      encoded->size());
        }
      }
    }
  }
}

// ---- 2a. 通道矩阵：PNG / TIFF 的 1/2/3/4 通道在 CPU 基线上逐位无损 ----
//
// 这四条通道数正好对应 PNG 的四种颜色类型（灰度 / 灰度+A / RGB / RGBA）。
// 少测一条，就等于默认「那张图会被悄悄降维」——而带 Alpha 的 PNG 在现实里
// 比不带的多得多。TIFF 另外要求 spp 与 ExtraSamples 同时写对，否则 4 通道
// 会被别的解码器读成 CMYK。
void testCpuChannelMatrix() {
  Router router;
  const EncodeParams params;

  for (int channels = 1; channels <= 4; ++channels) {
    const Image src = makeImage(97, 61, channels); // 故意是奇数尺寸

    for (Format format : {Format::Png, Format::Tiff}) {
      auto encoded = router.encodeWith(BackendKind::Cpu, format, src, params);
      check(static_cast<bool>(encoded), "channel matrix encode");
      if (!encoded) continue;

      auto decoded = router.decodeWith(BackendKind::Cpu, format,
                                       encoded->data(), encoded->size());
      check(static_cast<bool>(decoded), "channel matrix decode");
      if (!decoded) continue;

      check(decoded->channels == channels,
            "channel matrix keeps the sample count");
      check(decoded->width == src.width && decoded->height == src.height,
            "channel matrix keeps geometry");
      if (decoded->pixels != src.pixels) {
        // 只打印第一个不同的位置：通道排布写错时，光看「不相等」帮不上忙。
        size_t first = 0;
        while (first < src.pixels.size() &&
               decoded->pixels[first] == src.pixels[first]) {
          ++first;
        }
        std::printf("MISMATCH %s %dch at %zu: %u vs %u\n",
                    image_codec::offload::formatName(format), channels, first,
                    static_cast<unsigned>(src.pixels[first]),
                    static_cast<unsigned>(decoded->pixels[first]));
      }
      check(decoded->pixels == src.pixels, "channel matrix lossless");

      std::printf("cpu %-4s %dch roundtrip: %zu bytes\n",
                  image_codec::offload::formatName(format), channels,
                  encoded->size());
    }
  }
}

// ---- 2b. JPEG 没有 Alpha：2/4 通道必须被明确拒绝 ----
//
// 静默丢 Alpha 也能「编码成功」，只是成功得很危险：文件能打开、PSNR 也正常，
// 只有透明度凭空消失。因此这里要求两条路径都失败，且错误码是 InvalidArgument
// （参数本身就不成立，换后端也无济于事）。
void testJpegAlphaRejected() {
  Router router;
  EncodeParams params;
  params.jpeg_quality = 90;

  for (int channels : {2, 4}) {
    const Image src = makeImage(32, 16, channels);

    for (BackendKind kind : {BackendKind::Cpu, BackendKind::OpenCl}) {
      const auto encoded = router.encodeWith(kind, Format::Jpeg, src, params);
      check(!encoded, "jpeg must reject images with an alpha sample");
      if (!encoded) {
        // 未编进本构建的后端返回 BackendUnavailable：那不是「参数错了」，
        // 只是这一单它根本没被叫到。CPU-only 构建下这条断言不该红。
        if (encoded.error().code !=
            image_codec::offload::Status::BackendUnavailable) {
          check(encoded.error().code ==
                    image_codec::offload::Status::InvalidArgument,
                "jpeg alpha rejection is InvalidArgument");
        }
        std::printf("jpeg %dch on %-6s: rejected (%s)\n", channels,
                    image_codec::offload::backendName(kind),
                    encoded.error().message.c_str());
      }
    }

    // 自动路由同样不能靠「换一个后端」把 Alpha 弄丢。
    const auto encoded = router.encode(Format::Jpeg, src, params);
    check(!encoded, "jpeg auto route must not silently drop alpha");
  }
}

// ---- 2c. 跨后端互通：一个后端编出来的 4 通道文件，另一个后端要能读 ----
//
// 通道模型一旦在某一边写错（比如把 RGBA 当 RGB 铺开），单后端自测仍然会过，
// 只有交叉读取才会暴露。
void testFourChannelCrossBackend() {
  Router router;
  const Image src = makeImage(83, 47, 4);
  const EncodeParams params;

  for (BackendKind kind : router.order()) {
    if (kind == BackendKind::Cpu) continue;

    for (Format format : {Format::Png, Format::Tiff}) {
      const auto from_cpu =
          router.encodeWith(BackendKind::Cpu, format, src, params);
      check(static_cast<bool>(from_cpu), "4ch cpu encode");
      if (!from_cpu) continue;

      const auto on_gpu =
          router.decodeWith(kind, format, from_cpu->data(), from_cpu->size());
      if (!on_gpu) {
        std::printf("%s decode of cpu 4ch %s: %s\n",
                    image_codec::offload::backendName(kind),
                    image_codec::offload::formatName(format),
                    on_gpu.error().message.c_str());
      } else {
        check(on_gpu->channels == 4, "gpu reads 4ch from cpu");
        check(on_gpu->pixels == src.pixels, "gpu reads cpu 4ch losslessly");
      }

      const auto from_gpu = router.encodeWith(kind, format, src, params);
      if (!from_gpu) {
        std::printf("%s 4ch %s encode: %s\n",
                    image_codec::offload::backendName(kind),
                    image_codec::offload::formatName(format),
                    from_gpu.error().message.c_str());
        continue;
      }

      const auto on_cpu = router.decodeWith(BackendKind::Cpu, format,
                                            from_gpu->data(), from_gpu->size());
      check(static_cast<bool>(on_cpu), "cpu decode of gpu 4ch");
      if (on_cpu) {
        check(on_cpu->channels == 4, "cpu reads 4ch from gpu");
        check(on_cpu->pixels == src.pixels, "cpu reads gpu 4ch losslessly");
      }
    }
  }
}

// ---- 2d. GPU 后端往返（三个格式 × 1..4 通道）----
//
// 对每个已编译进来的非 CPU 后端都跑一遍，而不是只测 OpenCL：CUDA 曾经把
// 通道数写死 3、TIFF 容器拒收 2 通道，单后端自测发现不了「另一条路上悄悄降维」。
// 用**强制指定**的入口，自动回退会把「这个后端其实没接这一单」伪装成成功。
// 后端没编进来 / 本机没可用设备时跳过（打印原因），不算失败。
void testBackendRoundtrip() {
  Router router;
  EncodeParams params;

  for (BackendKind kind : router.order()) {
    if (kind == BackendKind::Cpu) continue;

    for (int channels = 1; channels <= 4; ++channels) {
      const Image src = makeImage(97, 61, channels); // 故意不是 16 的倍数

      // PNG / TIFF 无损：必须逐位一致，且通道数原样保留。
      for (Format format : {Format::Png, Format::Tiff}) {
        params.tiff_compression = 5;

        auto encoded = router.encodeWith(kind, format, src, params);
        if (!encoded) {
          std::printf("%s %s encode (%dch) unavailable: %s\n",
                      image_codec::offload::backendName(kind),
                      image_codec::offload::formatName(format), channels,
                      encoded.error().message.c_str());
          continue;
        }

        auto decoded =
            router.decodeWith(kind, format, encoded->data(), encoded->size());
        check(static_cast<bool>(decoded), "gpu decode");
        if (!decoded) continue;

        check(decoded->channels == channels, "gpu keeps channel count");
        check(decoded->width == src.width && decoded->height == src.height,
              "gpu keeps geometry");
        check(decoded->pixels == src.pixels, "gpu lossless roundtrip");

        std::printf("%s %-4s %dch roundtrip: %zu bytes\n",
                    image_codec::offload::backendName(kind),
                    image_codec::offload::formatName(format), channels,
                    encoded->size());
      }

      // JPEG 只有 1/3 通道；2/4 由 testJpegAlphaRejected 负责检查拒绝路径。
      if (channels != 1 && channels != 3) continue;

      // JPEG 有损：几何与通道必须对，质量用 PSNR 兜住下限。
      params.jpeg_quality = 92;
      params.jpeg_subsampling = 444; // 合成图案逐像素随机，4:2:0 测的是子采样
      auto encoded = router.encodeWith(kind, Format::Jpeg, src, params);
      if (!encoded) continue;

      auto decoded = router.decodeWith(kind, Format::Jpeg, encoded->data(),
                                       encoded->size());
      check(static_cast<bool>(decoded), "gpu jpeg decode");
      if (!decoded) continue;

      check(decoded->channels == channels, "gpu jpeg keeps channel count");
      check(decoded->width == src.width && decoded->height == src.height,
            "gpu jpeg keeps geometry");
      const auto psnr = image_codec::offload::metrics::psnr(src, *decoded);
      check(static_cast<bool>(psnr), "gpu jpeg psnr");
      if (psnr) {
        check(*psnr > 30.0, "gpu jpeg quality >= 30 dB at q=92");
        std::printf("%s jpeg %dch q=92: %.2f dB, %zu bytes\n",
                    image_codec::offload::backendName(kind), channels, *psnr,
                    encoded->size());
      }
    }
  }
}

// 4:2:2 子采样：色度平面与亮度等高，只做水平抽取。
//
// 这条路径容易被「4:2:0 已经对」掩盖过去：两个子采样共用一个下采样实现时，
// 把抽取比例写死成 2x2 也照样能编出能打开的文件——亮度正常、只有颜色系统性
// 跑偏。随机图案测不出来（色度本来就是噪声），所以这里用**平滑渐变图**：
// 写死 2x2 时下半幅色度被摊平，PSNR 会从 33 dB 掉到 15~19 dB。
// 同时要求 4:2:2 不低于 4:2:0 —— 平滑图上少丢垂直色度只会更好，反过来就是
// 「行取错了」。
void testJpegSubsampling() {
  Router router;

  Image grad;
  grad.width = 65; // 奇数宽度：色度平面最后一列走边缘复制分支
  grad.height = 39;
  grad.channels = 3;
  grad.pixels.resize(static_cast<size_t>(grad.width) * grad.height * 3);
  for (int y = 0; y < grad.height; ++y) {
    for (int x = 0; x < grad.width; ++x) {
      uint8_t *p = &grad.pixels[(static_cast<size_t>(y) * grad.width + x) * 3];
      p[0] = static_cast<uint8_t>((x * 4) & 0xFF);
      p[1] = static_cast<uint8_t>((y * 6) & 0xFF);
      p[2] = static_cast<uint8_t>(((x + y) * 2) & 0xFF);
    }
  }

  const auto quality_at = [&](BackendKind kind, int subsampling) -> double {
    EncodeParams params;
    params.jpeg_quality = 90;
    params.jpeg_subsampling = subsampling;
    const auto encoded = router.encodeWith(kind, Format::Jpeg, grad, params);
    if (!encoded) return -1.0;
    const auto decoded = router.decodeWith(BackendKind::Cpu, Format::Jpeg,
                                           encoded->data(), encoded->size());
    if (!decoded) return -1.0;
    const auto psnr = image_codec::offload::metrics::psnr(grad, *decoded);
    return psnr ? *psnr : -1.0;
  };

  for (BackendKind kind : router.order()) {
    if (kind == BackendKind::Cpu) continue;
    const double p420 = quality_at(kind, 420);
    const double p422 = quality_at(kind, 422);
    if (p422 < 0.0 || p420 < 0.0) {
      std::printf("%s jpeg 422: skipped (encode/decode unavailable)\n",
                  image_codec::offload::backendName(kind));
      continue;
    }
    check(p422 >= 30.0, "jpeg 422 quality >= 30 dB");
    check(p422 >= p420 - 1.0, "jpeg 422 not worse than 420");
    std::printf("%s jpeg 422: %.2f dB (4:2:0 %.2f dB)\n",
                image_codec::offload::backendName(kind), p422, p420);
  }
}

// ---- 3. 指标 golden（三个后端的共同基准）----

constexpr double kGoldenMse = 1.989219444444;
constexpr double kGoldenPsnr = 45.143976650968;
constexpr double kGoldenSsim = 0.992878374042;
constexpr double kGoldenMsSsim = 0.999443606477;
constexpr int kGoldenLevels = 5;

void testGolden() {
  const Image a = makeImage(400, 300, 3);
  const Image b = perturb(a, 99);

  const auto mse = image_codec::offload::metrics::mse(a, b);
  check(static_cast<bool>(mse), "golden mse");
  if (mse)
    check(std::fabs(*mse - kGoldenMse) < 1e-9, "golden mse matches CUDA");

  const auto psnr = image_codec::offload::metrics::psnr(a, b);
  check(static_cast<bool>(psnr), "golden psnr");
  if (psnr) {
    check(std::fabs(*psnr - kGoldenPsnr) < 1e-9, "golden psnr matches CUDA");
  }

  const auto ssim = image_codec::offload::metrics::ssim(a, b);
  check(static_cast<bool>(ssim), "golden ssim");
  if (ssim) {
    check(std::fabs(ssim->ssim - kGoldenSsim) < 1e-9,
          "golden ssim matches CUDA");
    check(std::fabs(ssim->ms_ssim - kGoldenMsSsim) < 1e-9,
          "golden ms-ssim matches CUDA");
    check(ssim->levels == kGoldenLevels, "golden levels matches CUDA");
  }
}

// 常数图对：sigma = 0，CS 恒为 1，SSIM 退化成亮度项，可以手算。
void testAnalytic() {
  Image a;
  a.width = 400;
  a.height = 300;
  a.channels = 3;
  a.pixels.assign(static_cast<size_t>(400) * 300 * 3, 100);

  Image b = a;
  std::fill(b.pixels.begin(), b.pixels.end(), 110);

  const double c1 = (0.01 * 255.0) * (0.01 * 255.0);
  const double expected =
      (2.0 * 100.0 * 110.0 + c1) / (100.0 * 100.0 + 110.0 * 110.0 + c1);

  const auto ssim = image_codec::offload::metrics::ssim(a, b);
  check(static_cast<bool>(ssim), "analytic ssim");
  if (!ssim) return;

  check(std::fabs(ssim->ssim - expected) < 1e-9,
        "ssim matches luminance-only formula");

  // CS 恒为 1，所以 MS-SSIM 只剩最末一级的 SSIM 按其归一化后的权重开方。
  const double weight_last =
      0.1333 / (0.0448 + 0.2856 + 0.3001 + 0.2363 + 0.1333);
  check(std::fabs(ssim->ms_ssim - std::pow(expected, weight_last)) < 1e-9,
        "ms-ssim matches single-scale term");

  const auto mse = image_codec::offload::metrics::mse(a, b);
  check(static_cast<bool>(mse), "analytic mse");
  if (mse) check(std::fabs(*mse - 100.0) < 1e-9, "mse == 10^2");
}

// ---- 4. 后端之间必须给出同一个数字 ----

void testBackendAgreement() {
  Router router;
  // 3 通道是常规路径，4 通道是「Alpha 有没有被指标偷偷算错」的那条：
  // 通道数写死 3 的实现会把 RGBA 的第 4 个样本整列忽略，数字看起来仍然合理。
  for (int channels : {3, 4}) {
    const Image a = makeImage(320, 240, channels);
    const Image b = perturb(a, 11);

    const auto ref_mse = router.mseWith(BackendKind::Cpu, a, b);
    const auto ref_psnr = router.psnrWith(BackendKind::Cpu, a, b);
    const auto ref_ssim = router.ssimWith(BackendKind::Cpu, a, b);
    check(static_cast<bool>(ref_mse) && static_cast<bool>(ref_psnr) &&
              static_cast<bool>(ref_ssim),
          "cpu reference metrics");

    for (BackendKind kind : router.order()) {
      if (kind == BackendKind::Cpu) continue;

      const auto mse = router.mseWith(kind, a, b);
      if (!mse) {
        std::printf("%s: unavailable (%s)\n",
                    image_codec::offload::backendName(kind),
                    mse.error().message.c_str());
        continue; // 未编译进来 / 无设备：不是失败
      }

      const auto psnr = router.psnrWith(kind, a, b);
      const auto ssim = router.ssimWith(kind, a, b);
      check(static_cast<bool>(psnr) && static_cast<bool>(ssim),
            "backend metrics complete");

      if (ref_mse) check(std::fabs(*mse - *ref_mse) < 1e-9, "mse matches CPU");
      if (ref_psnr && psnr) {
        check(std::fabs(*psnr - *ref_psnr) < 1e-9, "psnr matches CPU");
      }
      if (ref_ssim && ssim) {
        check(std::fabs(ssim->ssim - ref_ssim->ssim) < 1e-9,
              "ssim matches CPU");
        check(std::fabs(ssim->ms_ssim - ref_ssim->ms_ssim) < 1e-9,
              "ms-ssim matches CPU");
        check(ssim->levels == ref_ssim->levels, "levels match CPU");
      }

      std::printf("%s (%dch): mse=%.9f psnr=%.6f ssim=%.9f ms-ssim=%.9f\n",
                  image_codec::offload::backendName(kind), channels, *mse,
                  psnr ? *psnr : 0.0, ssim ? ssim->ssim : 0.0,
                  ssim ? ssim->ms_ssim : 0.0);
    }
  }
}

// ---- 5. 通道保真：1 通道进、1 通道出 ----
//
// 编码器里最容易悄悄犯的错是「顺手把灰度复制成 RGB」：体积翻三倍、通道数从 1
// 变 3，而肉眼几乎看不出来。因此这里不只测「能解出来」，而是要求**解码结果
// 仍是 1 通道**；不支持的后端必须明确拒绝并回退，不允许升维后假装成功。
void testGrayChannelFidelity() {
  Router router;
  const Image gray = makeImage(65, 35, 1);

  for (Format format : {Format::Png, Format::Tiff, Format::Jpeg}) {
    EncodeParams params;
    params.jpeg_quality = 95;
    params.jpeg_subsampling = 444;

    auto encoded = router.encodeWith(BackendKind::Cpu, format, gray, params);
    check(static_cast<bool>(encoded), "gray baseline encode");
    if (!encoded) continue;

    image_codec::offload::Outcome outcome;
    auto decoded =
        router.decode(format, encoded->data(), encoded->size(), &outcome);
    check(static_cast<bool>(decoded), "gray decode");
    if (!decoded) continue;

    check(decoded->channels == 1, "gray decode keeps 1 channel");
    check(decoded->width == gray.width && decoded->height == gray.height,
          "gray decode keeps geometry");
    std::printf("gray %-5s -> %s (%d channel(s))\n",
                image_codec::offload::formatName(format),
                image_codec::offload::backendName(outcome.used),
                decoded->channels);
  }

  // 强制指定 OpenCL：C ABI 现在支持 1 通道，因此应当**成功且仍是 1 通道**。
  // 若本构建没有 OpenCL（或本机无设备），退回「明确失败」也算通过——两种
  // 结果都不允许出现「悄悄升维成 3 通道的成功」。
  EncodeParams params;
  params.jpeg_quality = 95;
  auto jpeg = router.encodeWith(BackendKind::Cpu, Format::Jpeg, gray, params);
  if (jpeg) {
    const auto forced = router.decodeWith(BackendKind::OpenCl, Format::Jpeg,
                                          jpeg->data(), jpeg->size());
    if (forced) {
      check(forced->channels == 1,
            "forced opencl decode of gray must keep 1 channel");
    } else {
      std::printf("forced opencl on gray jpeg: %s\n",
                  forced.error().message.c_str());
    }
  }
}

// ---- 6. 回退必须留下原因 ----
//
// 找一个**真正**只有 CPU 会做的组合：OpenCL 后端只实现 TIFF 的 none/LZW，
// deflate（compression=8）没有对应路径。自动模式必须回退到 CPU 并记下原因；
// 强制模式必须失败，绝不换个实现冒充。
void testFallbackLeavesReason() {
  Router router;
  router.setOrder({BackendKind::OpenCl});

  const Image src = makeImage(64, 48, 3);

  EncodeParams params;
  params.tiff_compression = 8; // deflate：OpenCL 没有这条路径
  image_codec::offload::Outcome outcome;
  const auto encoded = router.encode(Format::Tiff, src, params, &outcome);
  check(static_cast<bool>(encoded), "tiff deflate via auto fallback");
  if (!encoded) return;

  check(outcome.used == BackendKind::Cpu, "tiff deflate fell back to cpu");
  check(!outcome.notes.empty(), "fallback recorded a reason");
  if (!outcome.notes.empty()) {
    std::printf("fallback note: %s\n", outcome.notes.front().c_str());
  }

  // 回退产物本身必须是对的：无损格式解回来要逐位一致。
  const auto decoded = router.decodeWith(BackendKind::Cpu, Format::Tiff,
                                         encoded->data(), encoded->size());
  check(static_cast<bool>(decoded), "fallback tiff decode");
  if (decoded) check(decoded->pixels == src.pixels, "fallback tiff lossless");

  // 强制指定同一个后端时必须失败（OpenCL 未编进本构建时同样是失败）。
  const auto forced =
      router.encodeWith(BackendKind::OpenCl, Format::Tiff, src, params);
  check(!forced, "forced backend never silently swaps implementations");
  if (!forced) {
    std::printf("forced opencl on tiff deflate: %s\n",
                forced.error().message.c_str());
  }
}

// ---- 7. C API 的 forced 入口必须真的只用那一个后端 ----
//
// 为什么单独一条：`set_order` 会把 CPU 永远排到最后（兜底不能丢），于是
// 「首选 CPU」实际仍会先跑 GPU。存图管线的一条 lane = 一个后端，CPU lane
// 必须真的是 CPU，否则两条 lane 会在同一块设备上互抢显存、统计也失真。
// 这里用一个「只有 CPU 会做」的组合（TIFF deflate）来区分两者。
void testForcedBackendApi() {
  const Image src = makeImage(64, 48, 3);

  ico_image image{};
  image.width = static_cast<uint32_t>(src.width);
  image.height = static_cast<uint32_t>(src.height);
  image.channels = static_cast<uint32_t>(src.channels);
  image.stride = src.rowBytes();
  image.data_size = src.pixels.size();
  image.data = const_cast<uint8_t *>(src.pixels.data());

  ico_encode_params params{};
  params.tiff_compression = 8; // deflate：只有 CPU 后端支持

  ico_codec *cpu = nullptr;
  check(image_codec_offload_create_forced(&cpu, ICO_BACKEND_CPU) == ICO_OK,
        "create_forced(cpu)");
  if (cpu != nullptr) {
    const size_t capacity = src.pixels.size() * 2;
    std::vector<uint8_t> out(capacity);
    size_t out_len = capacity;
    const ico_status status =
        image_codec_offload_encode(cpu, ICO_FORMAT_TIFF, &image, &params,
                                   out.data(), &out_len, out.size());
    check(status == ICO_OK, "forced cpu encodes tiff deflate");

    int used = ICO_BACKEND_AUTO;
    check(image_codec_offload_last_outcome(cpu, &used, nullptr, 0) == ICO_OK,
          "forced cpu last_outcome");
    check(used == ICO_BACKEND_CPU, "forced cpu really used cpu");

    // 同一单在自动模式下必须换成「能用 GPU 就跑 GPU」的行为，但这里只验证
    // forced 语义本身：CPU 能做的单，forced CPU 也必须能做，且不回退。
    image_codec_offload_destroy(cpu);
  }

  // forced GPU 遇到「该后端不支持」必须失败，绝不换实现冒充。
  ico_codec *opencl = nullptr;
  check(image_codec_offload_create_forced(&opencl, ICO_BACKEND_OPENCL) ==
            ICO_OK,
        "create_forced(opencl)");
  if (opencl != nullptr) {
    std::vector<uint8_t> out(src.pixels.size() * 2);
    size_t out_len = out.size();
    const ico_status status =
        image_codec_offload_encode(opencl, ICO_FORMAT_TIFF, &image, &params,
                                   out.data(), &out_len, out.size());
    check(status != ICO_OK, "forced opencl never swaps implementation");
    if (status != ICO_OK) {
      std::printf("forced opencl on tiff deflate: %s\n",
                  image_codec_offload_status_string(status));
    }
    image_codec_offload_destroy(opencl);
  }
}

// ---- 8. 进阶编码参数（P4） ----
//
// 这一组检查的不是「参数能传进去」，而是「参数真的落进了码流/容器」。传进去
// 却被后端丢掉是最坏的一类缺陷：文件能打开、像素看着也正常，只有该有的标记
// （SOF2 / Predictor / DPI）悄悄没了。

uint32_t readLe16(const std::vector<uint8_t> &bytes, size_t at) {
  return static_cast<uint32_t>(bytes[at]) |
         (static_cast<uint32_t>(bytes[at + 1]) << 8);
}

uint32_t readLe32(const std::vector<uint8_t> &bytes, size_t at) {
  return static_cast<uint32_t>(bytes[at]) |
         (static_cast<uint32_t>(bytes[at + 1]) << 8) |
         (static_cast<uint32_t>(bytes[at + 2]) << 16) |
         (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

// JPEG 段标记扫描：0xFF 后跟 marker。熵编码数据里的 0xFF 一定被填成 0xFF00，
// 所以除了 RSTn 之外不会误命中。
bool hasJpegMarker(const std::vector<uint8_t> &bytes, uint8_t marker) {
  for (size_t i = 0; i + 1 < bytes.size(); ++i) {
    if (bytes[i] == 0xFF && bytes[i + 1] == marker) return true;
  }
  return false;
}

// RST0..RST7（0xD0..0xD7）的个数。
int countJpegRestarts(const std::vector<uint8_t> &bytes) {
  int count = 0;
  for (size_t i = 0; i + 1 < bytes.size(); ++i) {
    if (bytes[i] == 0xFF && bytes[i + 1] >= 0xD0 && bytes[i + 1] <= 0xD7) {
      ++count;
    }
  }
  return count;
}

// 在**经典小端** TIFF 的 IFD0 里找标签。本库的写出路径固定产出这种布局，
// 因此这里不需要通用解析器——只要回答「标签在不在、值字段是多少」。
bool tiffFindTag(const std::vector<uint8_t> &bytes, uint16_t wanted,
                 uint32_t *value_field) {
  if (bytes.size() < 8) return false;
  if (bytes[0] != 'I' || bytes[1] != 'I' || bytes[2] != 42 || bytes[3] != 0) {
    return false;
  }
  const uint32_t ifd = readLe32(bytes, 4);
  if (static_cast<size_t>(ifd) + 2 > bytes.size()) return false;
  const uint32_t count = readLe16(bytes, ifd);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t entry = ifd + 2 + static_cast<size_t>(i) * 12;
    if (entry + 12 > bytes.size()) return false;
    if (readLe16(bytes, entry) != wanted) continue;
    if (value_field != nullptr) *value_field = readLe32(bytes, entry + 8);
    return true;
  }
  return false;
}

// XResolution / YResolution 是 RATIONAL（count=1），值字段存的是「分子/分母
// 在文件中的偏移」。
bool tiffRational(const std::vector<uint8_t> &bytes, uint16_t tag,
                  double *out) {
  uint32_t offset = 0;
  if (!tiffFindTag(bytes, tag, &offset)) return false;
  if (static_cast<size_t>(offset) + 8 > bytes.size()) return false;
  const uint32_t numerator = readLe32(bytes, offset);
  const uint32_t denominator = readLe32(bytes, offset + 4);
  if (denominator == 0) return false;
  *out = static_cast<double>(numerator) / static_cast<double>(denominator);
  return true;
}

std::vector<uint8_t> encodeCpu(Router &router, Format format,
                               const Image &image, const EncodeParams &params) {
  auto bytes = router.encodeWith(BackendKind::Cpu, format, image, params);
  if (!bytes) {
    std::printf("cpu encode %s failed: %s\n", formatName(format),
                bytes.error().message.c_str());
    return {};
  }
  return std::move(*bytes);
}

std::vector<uint8_t> decodeCpu(Router &router, Format format,
                               const std::vector<uint8_t> &bytes) {
  auto image =
      router.decodeWith(BackendKind::Cpu, format, bytes.data(), bytes.size());
  if (!image) return {};
  return image->pixels;
}

void testJpegAdvancedParams() {
  Router router;
  const Image src = makeImage(97, 65, 3); // 不是 MCU 的整数倍

  EncodeParams base;
  base.jpeg_quality = 90;
  base.jpeg_subsampling = 444;
  const auto baseline = encodeCpu(router, Format::Jpeg, src, base);
  check(!baseline.empty(), "jpeg baseline encode");
  check(hasJpegMarker(baseline, 0xC0), "baseline writes SOF0");
  check(!hasJpegMarker(baseline, 0xC2), "baseline is not progressive");
  check(countJpegRestarts(baseline) == 0, "baseline writes no RSTn");
  const auto baseline_pixels = decodeCpu(router, Format::Jpeg, baseline);
  check(!baseline_pixels.empty(), "baseline decodes");

  // 渐进式：必须是 SOF2，且仍能解回来。
  EncodeParams progressive = base;
  progressive.jpeg_progressive = true;
  const auto progressive_bytes =
      encodeCpu(router, Format::Jpeg, src, progressive);
  check(!progressive_bytes.empty(), "jpeg progressive encode");
  check(hasJpegMarker(progressive_bytes, 0xC2), "progressive writes SOF2");
  check(!hasJpegMarker(progressive_bytes, 0xC0), "progressive has no SOF0");
  check(progressive_bytes != baseline, "progressive changes the stream");
  const auto progressive_pixels =
      decodeCpu(router, Format::Jpeg, progressive_bytes);
  check(progressive_pixels.size() == baseline_pixels.size(),
        "progressive decodes to the same geometry");

  // 优化霍夫曼：只换熵表，量化表没动 → 解码像素必须与 baseline **完全一致**。
  EncodeParams optimized = base;
  optimized.jpeg_optimize_huffman = true;
  const auto optimized_bytes = encodeCpu(router, Format::Jpeg, src, optimized);
  check(!optimized_bytes.empty(), "jpeg optimize-huffman encode");
  check(optimized_bytes.size() < baseline.size(),
        "optimize-huffman shrinks the stream");
  check(decodeCpu(router, Format::Jpeg, optimized_bytes) == baseline_pixels,
        "optimize-huffman must not change decoded pixels");

  // RST 间隔：码流里真的出现 RSTn；像素同样不变。
  EncodeParams restarted = base;
  restarted.jpeg_restart_interval = 2;
  const auto restarted_bytes = encodeCpu(router, Format::Jpeg, src, restarted);
  check(!restarted_bytes.empty(), "jpeg restart-interval encode");
  check(countJpegRestarts(restarted_bytes) > 0, "restart-interval writes RSTn");
  check(decodeCpu(router, Format::Jpeg, restarted_bytes) == baseline_pixels,
        "restart-interval must not change decoded pixels");

  // 亮度/色度分开：量化表不同 → 码流不同（same quality 的等价性上面已由
  // baseline 覆盖，这里只验证「真的生效」）。
  EncodeParams split = base;
  split.jpeg_luma_quality = 30;
  split.jpeg_chroma_quality = 95;
  const auto split_bytes = encodeCpu(router, Format::Jpeg, src, split);
  check(!split_bytes.empty(), "jpeg luma/chroma quality encode");
  check(split_bytes != baseline, "luma/chroma quality changes the stream");

  // 灰度图只有亮度表：色度质量必须**完全无效**，亮度质量必须生效。
  // 这一条防的是「把 chroma 质量误写进标 0」这类错位。
  const Image gray = makeImage(64, 64, 1);
  const auto gray_base = encodeCpu(router, Format::Jpeg, gray, base);
  check(!gray_base.empty(), "gray jpeg encode");
  EncodeParams gray_chroma = base;
  gray_chroma.jpeg_chroma_quality = 20;
  check(encodeCpu(router, Format::Jpeg, gray, gray_chroma) == gray_base,
        "gray jpeg ignores chroma quality");
  EncodeParams gray_luma = base;
  gray_luma.jpeg_luma_quality = 20;
  check(encodeCpu(router, Format::Jpeg, gray, gray_luma) != gray_base,
        "gray jpeg honors luma quality");
}

void testTiffAdvancedParams() {
  Router router;
  const Image src = makeImage(97, 65, 3);

  EncodeParams base;
  base.tiff_compression = 5; // LZW
  const auto plain = encodeCpu(router, Format::Tiff, src, base);
  check(!plain.empty(), "tiff lzw encode");

  // 默认必须**不写** Predictor / 分辨率标签：这是「与旧写盘路径逐字节一致」
  // 的前提（多一个 12 字节的 IFD 条目就会让对拍失败）。
  check(!tiffFindTag(plain, 317, nullptr), "default writes no Predictor tag");
  check(!tiffFindTag(plain, 282, nullptr), "default writes no XResolution");
  check(!tiffFindTag(plain, 283, nullptr), "default writes no YResolution");
  check(!tiffFindTag(plain, 296, nullptr), "default writes no ResolutionUnit");

  // Predictor=2：标签写进去，LZW 前向差分本身无损。
  EncodeParams predicted = base;
  predicted.tiff_predictor = 2;
  const auto predicted_bytes = encodeCpu(router, Format::Tiff, src, predicted);
  uint32_t value = 0;
  check(tiffFindTag(predicted_bytes, 317, &value) && value == 2,
        "predictor=2 written to IFD");
  check(predicted_bytes != plain, "predictor changes the payload");
  check(decodeCpu(router, Format::Tiff, predicted_bytes) == src.pixels,
        "predictor stays lossless");

  // 显式 Predictor=1（none）与「不写」像素等价，但必然多恰好一个 IFD 条目。
  EncodeParams explicit_none = base;
  explicit_none.tiff_predictor = 1;
  const auto explicit_none_bytes =
      encodeCpu(router, Format::Tiff, src, explicit_none);
  check(tiffFindTag(explicit_none_bytes, 317, &value) && value == 1,
        "predictor=1 written when explicitly asked");
  check(explicit_none_bytes.size() == plain.size() + 12,
        "explicit predictor=none adds exactly one IFD entry");
  check(decodeCpu(router, Format::Tiff, explicit_none_bytes) == src.pixels,
        "explicit predictor=none stays lossless");

  // Predictor 只对**真的有预测器**的压缩方式有意义：libtiff 的 Predictor 字段
  // 是 tif_predict.c 在注册 LZW / Deflate 时合并进字段表的，COMPRESSION_NONE
  // 与 PackBits 下 `TIFFSetField` 会报 "Unknown tag 317" 并静默丢弃。请求
  // 必须因此变成 no-op（产物与不请求逐字节相同），绝不能多写一个无效标签。
  for (const int compression : {1, 32773}) {
    EncodeParams without_pred = base;
    without_pred.tiff_compression = compression;
    EncodeParams with_pred = without_pred;
    with_pred.tiff_predictor = 2;
    const auto plain_bytes =
        encodeCpu(router, Format::Tiff, src, without_pred);
    const auto pred_bytes = encodeCpu(router, Format::Tiff, src, with_pred);
    check(!plain_bytes.empty() && plain_bytes == pred_bytes,
          "predictor is a no-op for compressions without a predictor");
  }

  // 分辨率：给了才写；y 缺省跟随 x；单位默认英寸、可显式厘米。
  EncodeParams resolved = base;
  resolved.tiff_x_dpi = 300;
  resolved.tiff_y_dpi = 150;
  resolved.tiff_resolution_unit = 3;
  const auto resolved_bytes = encodeCpu(router, Format::Tiff, src, resolved);
  double x_dpi = 0.0;
  double y_dpi = 0.0;
  check(tiffRational(resolved_bytes, 282, &x_dpi) && x_dpi == 300.0,
        "x dpi = 300");
  check(tiffRational(resolved_bytes, 283, &y_dpi) && y_dpi == 150.0,
        "y dpi = 150");
  check(tiffFindTag(resolved_bytes, 296, &value) && value == 3,
        "resolution unit = centimeter");

  EncodeParams x_only = base;
  x_only.tiff_x_dpi = 300;
  const auto x_only_bytes = encodeCpu(router, Format::Tiff, src, x_only);
  check(tiffRational(x_only_bytes, 283, &y_dpi) && y_dpi == 300.0,
        "y dpi follows x when unset");
  check(tiffFindTag(x_only_bytes, 296, &value) && value == 2,
        "default resolution unit is inch");

  // PackBits：容器里的 Compression 标签必须是 32773，且必须能解回来。
  EncodeParams packbits = base;
  packbits.tiff_compression = 32773;
  const auto packbits_bytes = encodeCpu(router, Format::Tiff, src, packbits);
  check(!packbits_bytes.empty(), "packbits encode");
  check(tiffFindTag(packbits_bytes, 259, &value) && value == 32773,
        "compression tag = packbits");
  check(decodeCpu(router, Format::Tiff, packbits_bytes) == src.pixels,
        "packbits stays lossless");

  // TIFF 的 JPEG 压缩：libtiff 是否编入了 jpeg codec 是构建期事实。
  // 能写就必须写对；不能写就必须**明确失败**，绝不悄悄退化成 LZW。
  if (router.encodeCapabilities().tiff_jpeg_compression) {
    EncodeParams jpeg = base;
    jpeg.tiff_compression = 7;
    jpeg.tiff_jpeg_quality = 92;
    const auto jpeg_bytes = encodeCpu(router, Format::Tiff, src, jpeg);
    check(!jpeg_bytes.empty(), "tiff jpeg encode");
    check(tiffFindTag(jpeg_bytes, 259, &value) && value == 7,
          "compression tag = jpeg");
    const auto decoded = router.decodeWith(
        BackendKind::Cpu, Format::Tiff, jpeg_bytes.data(), jpeg_bytes.size());
    check(static_cast<bool>(decoded), "tiff jpeg decodes");
    if (decoded) {
      const auto quality = router.psnr(src, *decoded);
      check(quality && *quality > 25.0, "tiff jpeg quality is sane");
    }
  } else {
    EncodeParams jpeg = base;
    jpeg.tiff_compression = 7;
    const auto rejected =
        router.encodeWith(BackendKind::Cpu, Format::Tiff, src, jpeg);
    check(!rejected, "tiff jpeg rejected when libtiff has no jpeg codec");
  }
}

void testPngAdvancedParams() {
  Router router;
  const Image src = makeImage(97, 65, 3);

  EncodeParams base;
  base.png_compression = 6;
  const auto plain = encodeCpu(router, Format::Png, src, base);
  check(!plain.empty(), "png default encode");
  check(decodeCpu(router, Format::Png, plain) == src.pixels,
        "png default stays lossless");

  // 五种 zlib 策略都必须产出**无损**的合法 PNG。策略只影响压缩路径，
  // 任何一条丢字节的实现都会在这一步暴露。
  for (int strategy : {0, 1, 2, 3, 4}) {
    EncodeParams params = base;
    params.png_strategy = strategy;
    const auto bytes = encodeCpu(router, Format::Png, src, params);
    check(!bytes.empty(), "png strategy encode");
    if (bytes.empty()) continue;
    check(decodeCpu(router, Format::Png, bytes) == src.pixels,
          "png strategy stays lossless");
  }

  // huffman-only 完全不找 LZ 匹配，在有噪声的图上必然与默认策略不同。
  EncodeParams huffman = base;
  huffman.png_strategy = 2;
  check(encodeCpu(router, Format::Png, src, huffman) != plain,
        "png huffman-only differs from the default strategy");
}

// 能力表与实现的**双向**契约：
//   * 实现拒绝 → 能力表也必须拒绝，且原因里必须点名是哪一项；
//   * 实现接受 → 能力表也必须承认（否则 UI 会把可用组合灰掉）。
void testCapabilityContract() {
  Router router;
  const Image src = makeImage(64, 48, 3);

  struct Probe {
    Format format;
    EncodeParams params;
    const char *item;
  };
  std::vector<Probe> probes;
  {
    EncodeParams params;
    params.jpeg_progressive = true;
    probes.push_back({Format::Jpeg, params, "progressive"});
  }
  {
    EncodeParams params;
    params.jpeg_optimize_huffman = true;
    probes.push_back({Format::Jpeg, params, "optimize-huffman"});
  }
  {
    EncodeParams params;
    params.jpeg_restart_interval = 2;
    probes.push_back({Format::Jpeg, params, "restart-interval"});
  }
  {
    EncodeParams params;
    params.jpeg_luma_quality = 70;
    probes.push_back({Format::Jpeg, params, "luma/chroma-quality"});
  }
  {
    EncodeParams params;
    params.tiff_predictor = 2;
    probes.push_back({Format::Tiff, params, "predictor"});
  }
  {
    EncodeParams params;
    params.tiff_x_dpi = 300;
    probes.push_back({Format::Tiff, params, "resolution"});
  }
  {
    EncodeParams params;
    params.tiff_compression = 32773;
    probes.push_back({Format::Tiff, params, "packbits"});
  }
  {
    EncodeParams params;
    params.png_strategy = 3;
    probes.push_back({Format::Png, params, "strategy"});
  }

  // 自动模式取并集：CPU 永远在最后兜底，所以这组参数必须全部「可表达」。
  const auto union_caps = router.encodeCapabilities();
  check(union_caps.jpeg_progressive && union_caps.jpeg_optimize_huffman &&
            union_caps.jpeg_restart_interval &&
            union_caps.jpeg_luma_chroma_quality && union_caps.tiff_predictor &&
            union_caps.tiff_resolution && union_caps.tiff_packbits &&
            union_caps.png_strategy,
        "auto mode can express every advanced param (cpu is the backstop)");

  int rejections = 0;
  for (BackendKind kind :
       {BackendKind::OpenCl, BackendKind::Cuda, BackendKind::Vulkan}) {
    for (const Probe &probe : probes) {
      const auto verdict =
          router.checkEncodeParams(kind, probe.format, probe.params);
      const auto encoded =
          router.encodeWith(kind, probe.format, src, probe.params);

      if (encoded) {
        check(static_cast<bool>(verdict),
              "backend that accepts must also declare the capability");
        continue;
      }
      // 没编进本构建 / 本机没设备：与能力无关，跳过。
      if (encoded.error().code == Status::BackendUnavailable) continue;
      if (encoded.error().code != Status::UnsupportedFormat) continue;

      ++rejections;
      check(!verdict, "a rejected request must fail the capability check too");
      if (!verdict && verdict.error().message.find(
                          "encode params not supported") != std::string::npos) {
        check(verdict.error().message.find(probe.item) != std::string::npos,
              "capability rejection names the missing parameter");
      }
    }
  }
  std::printf("advanced params rejected by gpu backends: %d\n", rejections);
}

// 强制句柄的能力表必须是**那个后端自己的**，而不是当前顺序的并集。
//
// 这里真的错过一次：`ico_encode_capabilities` 对 forced 句柄也调用
// `Router::encodeCapabilities()`（并集），于是 OpenCL lane 报告「支持渐进式 /
// PNG 策略」，存图调度器据此把任务派给它，`encodeWith` 又把它拒掉——表现是
// 「这条 lane 接了活，一张也没写出来」，而能力表看上去一切正常。
void testForcedCapabilitiesMatchBackend() {
  const Image src = makeImage(64, 48, 3);

  ico_image image{};
  image.width = static_cast<uint32_t>(src.width);
  image.height = static_cast<uint32_t>(src.height);
  image.channels = static_cast<uint32_t>(src.channels);
  image.stride = static_cast<uint32_t>(src.rowBytes());
  image.data_size = src.pixels.size();
  image.data = const_cast<uint8_t *>(src.pixels.data());

  const auto matches = [](const ico_encode_capabilities &c,
                          const EncodeCapabilities &ref) {
    return c.jpeg_progressive == (ref.jpeg_progressive ? 1 : 0) &&
           c.jpeg_optimize_huffman == (ref.jpeg_optimize_huffman ? 1 : 0) &&
           c.jpeg_restart_interval == (ref.jpeg_restart_interval ? 1 : 0) &&
           c.jpeg_luma_chroma_quality ==
               (ref.jpeg_luma_chroma_quality ? 1 : 0) &&
           c.tiff_predictor == (ref.tiff_predictor ? 1 : 0) &&
           c.tiff_resolution == (ref.tiff_resolution ? 1 : 0) &&
           c.tiff_packbits == (ref.tiff_packbits ? 1 : 0) &&
           c.tiff_jpeg_compression == (ref.tiff_jpeg_compression ? 1 : 0) &&
           c.png_strategy == (ref.png_strategy ? 1 : 0) &&
           c.tiff_strip_sharding == (ref.tiff_strip_sharding ? 1 : 0);
  };

  Router router;
  const auto available = router.availableBackends();
  const std::pair<int, BackendKind> kinds[] = {
      {ICO_BACKEND_CPU, BackendKind::Cpu},
      {ICO_BACKEND_OPENCL, BackendKind::OpenCl},
      {ICO_BACKEND_CUDA, BackendKind::Cuda},
      {ICO_BACKEND_VULKAN, BackendKind::Vulkan},
  };

  for (const auto &[api_backend, kind] : kinds) {
    ico_codec *codec = nullptr;
    check(image_codec_offload_create_forced(&codec, api_backend) == ICO_OK,
          "create_forced for every backend value");
    if (codec == nullptr) continue;

    ico_encode_capabilities caps{};
    check(image_codec_offload_encode_capabilities(codec, &caps) == ICO_OK,
          "forced encode_capabilities");
    check(matches(caps, router.encodeCapabilitiesFor(kind)),
          "forced capability table equals that backend's own table");

    // 未编进本构建的后端：能力表必须是全 0，而不是拿 CPU 的表顶替。
    bool built = false;
    for (BackendKind entry : available) {
      built = built || entry == kind;
    }
    if (!built) {
      check(matches(caps, EncodeCapabilities{}),
            "unbuilt forced backend reports no capabilities");
    }

    // 能力表说「不支持」的参数，C API 编码必须真的拒绝：两张表不能分开走。
    const EncodeCapabilities own = router.encodeCapabilitiesFor(kind);
    if (built && !own.jpeg_progressive) {
      ico_encode_params params{};
      params.jpeg_progressive = 1;
      std::vector<uint8_t> out(src.pixels.size() * 4);
      size_t out_len = out.size();
      const ico_status status = image_codec_offload_encode(
          codec, ICO_FORMAT_JPEG, &image, &params, out.data(), &out_len,
          out.size());
      check(status == ICO_ERR_UNSUPPORTED_FORMAT,
            "forced backend rejects advanced params it does not declare");
    }
    image_codec_offload_destroy(codec);
  }
}

// C API 的同一组参数：既要**真的落进码流**，又要保证「清零结构体」等于
// 「内部默认值」——老调用方不写新字段时，产物必须与升级前逐字节相同。
void testCapiAdvancedParams() {
  const Image src = makeImage(64, 48, 3);

  ico_image image{};
  image.width = static_cast<uint32_t>(src.width);
  image.height = static_cast<uint32_t>(src.height);
  image.channels = static_cast<uint32_t>(src.channels);
  image.stride = static_cast<uint32_t>(src.rowBytes());
  image.data_size = src.pixels.size();
  image.data = const_cast<uint8_t *>(src.pixels.data());

  ico_codec *cpu = nullptr;
  check(image_codec_offload_create_forced(&cpu, ICO_BACKEND_CPU) == ICO_OK,
        "create_forced(cpu)");
  if (cpu == nullptr) return;

  ico_encode_capabilities caps{};
  check(image_codec_offload_encode_capabilities(cpu, &caps) == ICO_OK,
        "encode_capabilities(cpu)");
  check(caps.jpeg_progressive == 1 && caps.jpeg_optimize_huffman == 1 &&
            caps.jpeg_restart_interval == 1 &&
            caps.jpeg_luma_chroma_quality == 1 && caps.tiff_predictor == 1 &&
            caps.tiff_resolution == 1 && caps.tiff_packbits == 1 &&
            caps.png_strategy == 1,
        "cpu declares every advanced param");
  check(image_codec_offload_encode_capabilities(cpu, nullptr) ==
            ICO_ERR_INVALID_ARGUMENT,
        "encode_capabilities rejects null out");
  check(image_codec_offload_encode_capabilities(nullptr, &caps) ==
            ICO_ERR_INVALID_ARGUMENT,
        "encode_capabilities rejects null codec");

  const auto encode = [&](int ico_format, const ico_encode_params &params) {
    std::vector<uint8_t> out(src.pixels.size() * 4);
    size_t out_len = out.size();
    const ico_status status = image_codec_offload_encode(
        cpu, ico_format, &image, &params, out.data(), &out_len, out.size());
    if (status != ICO_OK) {
      std::printf("c api encode(format=%d) failed: %s\n", ico_format,
                  image_codec_offload_status_string(status));
      return std::vector<uint8_t>{};
    }
    out.resize(out_len);
    return out;
  };

  ico_encode_params mode_params{};
  const auto mode_bytes = encode(ICO_FORMAT_JPEG, mode_params);
  check(!mode_bytes.empty(), "mode query has a successful encode");
  int mode = ICO_EXECUTION_UNAVAILABLE;
  check(image_codec_offload_last_execution_mode(cpu, &mode) == ICO_OK,
        "last execution mode query");
  check(mode == ICO_EXECUTION_CPU, "forced CPU reports CPU execution mode");
  check(image_codec_offload_last_execution_mode(nullptr, &mode) ==
            ICO_ERR_INVALID_ARGUMENT,
        "execution mode query rejects null codec");

  // 清零结构体 == 内部默认值：JPEG / TIFF 成立（这些字段里 0 不是合法取值）。
  Router router;
  const std::pair<int, Format> zeroed_formats[] = {
      {ICO_FORMAT_JPEG, Format::Jpeg},
      {ICO_FORMAT_TIFF, Format::Tiff},
  };
  for (const auto &[ico_format, format] : zeroed_formats) {
    ico_encode_params zeroed{};
    const auto via_c = encode(ico_format, zeroed);
    const auto via_cpp =
        router.encodeWith(BackendKind::Cpu, format, src, EncodeParams{});
    check(!via_c.empty(), "c api encode with zeroed params");
    check(via_cpp && via_c == *via_cpp,
          "zeroed c params must equal internal defaults");
  }

  // PNG 刻意不对称：等级 0 与「不滤波」都是合法取值，所以「未指定」是 -1。
  ico_encode_params png_defaults{};
  png_defaults.png_compression = -1;
  png_defaults.png_filter = -1;
  const auto png_via_c = encode(ICO_FORMAT_PNG, png_defaults);
  const auto png_via_cpp = router.encodeWith(BackendKind::Cpu, Format::Png,
                                             src, EncodeParams{});
  check(!png_via_c.empty(), "c api png encode with explicit -1 defaults");
  check(png_via_cpp && png_via_c == *png_via_cpp,
        "explicit -1 must equal internal png defaults");

  // 清零结构体（= 等级 0 + 不滤波）必须**真的生效**，而且仍然无损。
  // 这一条防的是「0 被当成未指定」的旧缺陷：两个合法取值永远传不进去。
  ico_encode_params png_literal{};
  const auto png_literal_bytes = encode(ICO_FORMAT_PNG, png_literal);
  check(!png_literal_bytes.empty(), "c api png level 0 / filter none encode");
  check(png_literal_bytes != png_via_c,
        "png level 0 + filter none really applies");
  {
    const auto decoded = router.decodeWith(BackendKind::Cpu, Format::Png,
                                           png_literal_bytes.data(),
                                           png_literal_bytes.size());
    check(static_cast<bool>(decoded) && decoded->pixels == src.pixels,
          "png level 0 stays lossless");
  }

  ico_encode_params jpeg{};
  jpeg.jpeg_progressive = 1;
  jpeg.jpeg_optimize_huffman = 1;
  jpeg.jpeg_restart_interval = 2;
  const auto jpeg_bytes = encode(ICO_FORMAT_JPEG, jpeg);
  check(hasJpegMarker(jpeg_bytes, 0xC2), "c api progressive reaches SOF2");
  check(countJpegRestarts(jpeg_bytes) > 0,
        "c api restart interval reaches RSTn");

  ico_encode_params tiff{};
  tiff.tiff_predictor = 2;
  tiff.tiff_x_dpi = 600;
  tiff.tiff_resolution_unit = 2;
  const auto tiff_bytes = encode(ICO_FORMAT_TIFF, tiff);
  uint32_t value = 0;
  double dpi = 0.0;
  check(tiffFindTag(tiff_bytes, 317, &value) && value == 2,
        "c api predictor reaches the tag");
  check(tiffRational(tiff_bytes, 282, &dpi) && dpi == 600.0,
        "c api dpi reaches the tag");

  ico_encode_params png{};
  png.png_strategy = 2;
  check(!encode(ICO_FORMAT_PNG, png).empty(), "c api png strategy encode");

  image_codec_offload_destroy(cpu);
}


// ---- TIFF 分片：与整图编码**逐字节相同** ----
//
// 分片的合法性全靠这一点：把一段行独立压成一条 strip，得到的压缩流必须与
// 「整图、RowsPerStrip = 片高」编码时对应那条逐位相同。只要有一条不成立，
// 拼出来的文件就会同时包含两种压缩参数（LZW 字典/Predictor 状态按 strip
// 重置，但参数字节流不同）——文件能打开、像素看着也对，只有校验和变了。
//
// 这里同时验三件事：
//   1. 单 lane 串行分片 == 整图编码（字节级）；
//   2. 多 lane 并行分片（每 lane 自己的句柄）== 同一个结果（fan-in 顺序无关）；
//   3. 声明不了分片的强制句柄必须**明确拒绝**，而不是拼一个残缺文件。
void testTiffStripSharding() {
  const Image src = makeImage(96, 80, 3);
  Router router;

  ico_image image{};
  image.width = static_cast<uint32_t>(src.width);
  image.height = static_cast<uint32_t>(src.height);
  image.channels = static_cast<uint32_t>(src.channels);
  image.stride = static_cast<uint32_t>(src.rowBytes());
  image.data_size = src.pixels.size();
  image.data = const_cast<uint8_t *>(src.pixels.data());

  ico_codec *cpu = nullptr;
  check(image_codec_offload_create_forced(&cpu, ICO_BACKEND_CPU) == ICO_OK,
        "strip sharding: create_forced(cpu)");
  if (cpu == nullptr) return;

  ico_encode_capabilities caps{};
  check(image_codec_offload_encode_capabilities(cpu, &caps) == ICO_OK &&
            caps.tiff_strip_sharding == 1,
        "cpu declares tiff strip sharding");

  struct Case {
    int compression;
    int predictor; // -1 = 不写标签
    int rows_per_strip;
    int channels;
    int dpi;
  };
  std::vector<Case> cases;
  for (const int channels : {1, 3, 4}) {
    for (const int compression : {1, 5, 8, 32773}) {
      for (const int predictor : {-1, 2}) {
        for (const int rps : {16, 31}) {
          cases.push_back({compression, predictor, rps, channels,
                           (rps == 31 ? 300 : 0)});
        }
      }
    }
  }

  int compared = 0;
  for (const Case &item : cases) {
    const Image frame = (item.channels == 3) ? src : makeImage(96, 80, item.channels);

    ico_image frame_image{};
    frame_image.width = static_cast<uint32_t>(frame.width);
    frame_image.height = static_cast<uint32_t>(frame.height);
    frame_image.channels = static_cast<uint32_t>(frame.channels);
    frame_image.stride = static_cast<uint32_t>(frame.rowBytes());
    frame_image.data_size = frame.pixels.size();
    frame_image.data = const_cast<uint8_t *>(frame.pixels.data());

    ico_encode_params params{};
    params.tiff_compression = item.compression;
    params.tiff_rows_per_strip = item.rows_per_strip;
    params.tiff_predictor = item.predictor;
    params.tiff_x_dpi = item.dpi;
    params.tiff_y_dpi = item.dpi;

    // 参照：整图一次编码（同参数、同 RowsPerStrip）。
    EncodeParams internal;
    internal.tiff_compression = item.compression;
    internal.tiff_rows_per_strip = item.rows_per_strip;
    internal.tiff_predictor = item.predictor;
    internal.tiff_x_dpi = item.dpi;
    internal.tiff_y_dpi = item.dpi;
    const auto whole = router.encodeWith(BackendKind::Cpu, Format::Tiff, frame,
                                         internal);
    if (!whole) {
      std::printf("strip sharding: whole encode failed (comp=%d)\n",
                  item.compression);
      check(false, "strip sharding: reference encode must succeed");
      continue;
    }

    const int strip_count =
        (frame.height + item.rows_per_strip - 1) / item.rows_per_strip;
    std::vector<std::vector<uint8_t>> payloads;
    for (int index = 0; index < strip_count; ++index) {
      const int first_row = index * item.rows_per_strip;
      const int rows = (first_row + item.rows_per_strip <= frame.height)
                           ? item.rows_per_strip
                           : (frame.height - first_row);
      std::vector<uint8_t> buffer(frame.pixels.size() + 4096);
      size_t length = buffer.size();
      const ico_status status = image_codec_offload_tiff_strip_encode(
          cpu, &frame_image, first_row, rows, &params, buffer.data(), &length,
          buffer.size());
      if (status != ICO_OK) {
        std::printf("strip sharding: strip encode failed (comp=%d ch=%d s=%d)\n",
                    item.compression, item.channels, status);
        check(false, "strip encode must succeed for a supported compression");
        payloads.clear();
        break;
      }
      buffer.resize(length);
      payloads.push_back(std::move(buffer));
    }
    if (payloads.empty()) continue;

    std::vector<const void *> pointers;
    std::vector<size_t> sizes;
    for (const auto &payload : payloads) {
      pointers.push_back(payload.data());
      sizes.push_back(payload.size());
    }

    ico_tiff_assembly assembly{};
    assembly.width = frame.width;
    assembly.height = frame.height;
    assembly.channels = frame.channels;
    assembly.rows_per_strip = item.rows_per_strip;
    assembly.params = params;

    std::vector<uint8_t> assembled(whole->size() + 4096);
    size_t assembled_len = assembled.size();
    const ico_status status = image_codec_offload_tiff_assemble(
        &assembly, pointers.data(), sizes.data(), static_cast<int>(sizes.size()),
        assembled.data(), &assembled_len, assembled.size());
    check(status == ICO_OK, "tiff assemble succeeds");
    if (status != ICO_OK) continue;
    assembled.resize(assembled_len);

    if (assembled != *whole) {
      std::printf(
          "strip sharding mismatch: comp=%d pred=%d rps=%d ch=%d dpi=%d "
          "(%zu vs %zu bytes)\n",
          item.compression, item.predictor, item.rows_per_strip, item.channels,
          item.dpi, assembled.size(), whole->size());
    }
    check(assembled == *whole,
          "strip sharding is byte-identical to whole-image encoding");
    ++compared;
  }
  std::printf("tiff strip sharding: %d case(s) byte-identical\n", compared);

  // 参数非法 / 不支持的组合必须**拒绝**，不能拼出残缺文件。
  {
    ico_encode_params tiled{};
    tiled.tiff_compression = 5;
    tiled.tiff_tile_size = 256; // tiled 布局用 Tile* 数组，与 Strip* 无关
    std::vector<uint8_t> buffer(1024);
    size_t length = buffer.size();
    check(image_codec_offload_tiff_strip_encode(
              cpu, &image, 0, 16, &tiled, buffer.data(), &length,
              buffer.size()) == ICO_ERR_UNSUPPORTED_FORMAT,
          "tiled layout is rejected for strip sharding");

    ico_encode_params jpeg_in_tiff{};
    jpeg_in_tiff.tiff_compression = 7; // 依赖 JPEGTABLES 共享量化表
    length = buffer.size();
    check(image_codec_offload_tiff_strip_encode(cpu, &image, 0, 16,
                                                &jpeg_in_tiff, buffer.data(),
                                                &length,
                                                buffer.size()) ==
              ICO_ERR_UNSUPPORTED_FORMAT,
          "jpeg-in-tiff is rejected for strip sharding");

    ico_encode_params plain{};
    plain.tiff_compression = 5;
    length = buffer.size();
    check(image_codec_offload_tiff_strip_encode(cpu, &image, 60, 40, &plain,
                                                buffer.data(), &length,
                                                buffer.size()) ==
              ICO_ERR_INVALID_ARGUMENT,
          "out-of-range strip row range is rejected");

    std::vector<std::vector<uint8_t>> one;
    one.emplace_back(16);
    std::vector<const void *> pointers{one[0].data()};
    std::vector<size_t> sizes{one[0].size()};
    ico_tiff_assembly assembly{};
    assembly.width = image.width;
    assembly.height = image.height;
    assembly.channels = image.channels;
    assembly.rows_per_strip = 16; // 80 行 / 16 = 5 条，这里只给 1 条
    assembly.params = plain;
    std::vector<uint8_t> out(4096);
    size_t out_len = out.size();
    check(image_codec_offload_tiff_assemble(
              &assembly, pointers.data(), sizes.data(), 1, out.data(), &out_len,
              out.size()) == ICO_ERR_INVALID_ARGUMENT,
          "assemble rejects a strip count that does not match rows_per_strip");
  }

  // 多 lane 并行：每个 lane 用**自己的**句柄（C API 不保证句柄可并发）。
  // 分片之间互不依赖，fan-in 按行顺序拼装，结果必须与串行逐字节相同。
  {
    const int rows_per_strip = 8; // 80 行 / 8 = 10 片，够多个 lane 抢
    ico_encode_params params{};
    params.tiff_compression = 5;
    params.tiff_predictor = 2;
    params.tiff_rows_per_strip = rows_per_strip;
    params.tiff_x_dpi = 300;
    params.tiff_y_dpi = 300;

    EncodeParams internal;
    internal.tiff_compression = 5;
    internal.tiff_predictor = 2;
    internal.tiff_rows_per_strip = rows_per_strip;
    internal.tiff_x_dpi = 300;
    internal.tiff_y_dpi = 300;
    const auto whole =
        router.encodeWith(BackendKind::Cpu, Format::Tiff, src, internal);
    check(static_cast<bool>(whole), "parallel sharding: reference encode");

    const int strip_count = src.height / rows_per_strip;
    std::vector<std::vector<uint8_t>> payloads(
        static_cast<size_t>(strip_count));
    const int lanes = 4;
    std::vector<std::thread> workers;
    for (int lane = 0; lane < lanes; ++lane) {
      workers.emplace_back([&, lane]() {
        ico_codec *codec = nullptr;
        if (image_codec_offload_create_forced(&codec, ICO_BACKEND_CPU) !=
                ICO_OK ||
            codec == nullptr) {
          return;
        }
        ico_image local{};
        local.width = static_cast<uint32_t>(src.width);
        local.height = static_cast<uint32_t>(src.height);
        local.channels = static_cast<uint32_t>(src.channels);
        local.stride = static_cast<uint32_t>(src.rowBytes());
        local.data_size = src.pixels.size();
        local.data = const_cast<uint8_t *>(src.pixels.data());
        for (int index = lane; index < strip_count; index += lanes) {
          std::vector<uint8_t> buffer(src.pixels.size() / strip_count + 4096);
          size_t length = buffer.size();
          if (image_codec_offload_tiff_strip_encode(
                  codec, &local, index * rows_per_strip, rows_per_strip,
                  &params, buffer.data(), &length, buffer.size()) != ICO_OK) {
            payloads[static_cast<size_t>(index)].clear();
            continue;
          }
          buffer.resize(length);
          payloads[static_cast<size_t>(index)] = std::move(buffer);
        }
        image_codec_offload_destroy(codec);
      });
    }
    for (auto &worker : workers) worker.join();

    std::vector<const void *> pointers;
    std::vector<size_t> sizes;
    bool complete = true;
    for (const auto &payload : payloads) {
      complete = complete && !payload.empty();
      pointers.push_back(payload.data());
      sizes.push_back(payload.size());
    }
    check(complete, "every parallel strip produced bytes");
    if (complete && whole) {
      ico_tiff_assembly assembly{};
      assembly.width = src.width;
      assembly.height = src.height;
      assembly.channels = src.channels;
      assembly.rows_per_strip = rows_per_strip;
      assembly.params = params;
      std::vector<uint8_t> out(whole->size() + 4096);
      size_t out_len = out.size();
      check(image_codec_offload_tiff_assemble(
                &assembly, pointers.data(), sizes.data(), strip_count,
                out.data(), &out_len, out.size()) == ICO_OK,
            "parallel sharding: assemble");
      out.resize(out_len);
      check(out == *whole,
            "parallel sharding is byte-identical to whole-image encoding");
    }
  }

  // 声明不了分片的后端：强制句柄必须明确拒绝（绝不拼残缺文件）。
  {
    ico_codec *gpu = nullptr;
    check(image_codec_offload_create_forced(&gpu, ICO_BACKEND_OPENCL) == ICO_OK,
          "strip sharding: create_forced(opencl)");
    if (gpu != nullptr) {
      ico_encode_capabilities gpu_caps{};
      if (image_codec_offload_encode_capabilities(gpu, &gpu_caps) == ICO_OK) {
        check(gpu_caps.tiff_strip_sharding == 0,
              "gpu backends do not declare tiff strip sharding");
      }
      ico_encode_params params{};
      params.tiff_compression = 5;
      std::vector<uint8_t> buffer(1024);
      size_t length = buffer.size();
      const ico_status status = image_codec_offload_tiff_strip_encode(
          gpu, &image, 0, 16, &params, buffer.data(), &length, buffer.size());
      // 没编进本构建 → BackendUnavailable（能力表全 0）；编进来了 → 一定
      // 是 UnsupportedFormat。两者都不能是 OK。
      check(status == ICO_ERR_BACKEND_UNAVAILABLE ||
                status == ICO_ERR_UNSUPPORTED_FORMAT,
            "forced non-sharding backend rejects strip encode");
      image_codec_offload_destroy(gpu);
    }
  }

  image_codec_offload_destroy(cpu);
}

} // namespace

int main() {
  testFormatCatalogAndProbe();
  testExecutionModeContract();
  testRuntimeResolutionLimitContract();
  testJpeg2000CpuRoundtrip();
  testJpeg2000RowStreaming();
  testJpeg2000RowStreamingCapi();
  testJpeg2000RowStreamingCapiPropagatesReaderError();
  testJpegXlCpuRoundtrip();
  testGpuHybridNewFormatRoundtrip();
  testBmpRoundtrip();
  testCpuRoundtrip();
  testCpuChannelMatrix();
  testJpegAlphaRejected();
  testFourChannelCrossBackend();
  testBackendRoundtrip();
  testJpegSubsampling();
  testGolden();
  testAnalytic();
  testBackendAgreement();
  testGrayChannelFidelity();
  testFallbackLeavesReason();
  testForcedBackendApi();
  testJpegAdvancedParams();
  testTiffAdvancedParams();
  testPngAdvancedParams();
  testCapabilityContract();
  testForcedCapabilitiesMatchBackend();
  testCapiAdvancedParams();
  testTiffStripSharding();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}
