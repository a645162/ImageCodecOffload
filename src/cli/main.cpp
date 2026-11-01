/*
 * File: main.cpp
 * Brief: Main.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// main.cpp - ico：ImageCodecOffload 的命令行入口。
//
// 它同时是「本库能不能用」的最小证据与调参外壳：
//   ico backends                 每个后端的可用性与具体原因
//   ico info <file>              格式/尺寸（不改一个像素）
//   ico encode <in.bmp> <out>    编码，打印实际用时与实际后端
//   ico decode <in> <out.bmp>    解码，打印实际用时与实际后端
//   ico compare <a> <b>          逐后端给出 MSE/PSNR/SSIM/MS-SSIM 并互相对拍
//
// 为什么 compare 要逐后端跑一遍：SSIM 的常数、窗、边界策略、金字塔方式只要
// 有一处不同，两个后端的数字就会稳定地差在 1e-3 量级——肉眼看不出来，但横向
// 比较全部作废。这条命令就是那条护栏，别把它改成「随便用一个后端算一下」。
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ImageCodecOffload/image_codec_offload.h"
#include "bmp_io.h"
#include "image_codec_types.h"
#include "router.h"

namespace {

using image_codec::offload::BackendKind;
using image_codec::offload::EncodeParams;
using image_codec::offload::Format;
using image_codec::offload::Image;
using image_codec::offload::Router;

int g_failures = 0;

void printUsage() {
  std::printf(
      "ico - ImageCodecOffload CLI\n"
      "\n"
      "  ico backends\n"
      "  ico info <file>\n"
      "  ico encode <in> <out> [--quality N] [--subsampling 444|420|422]\n"
      "                        [--compression N] [--tile N] [--rows N]\n"
      "                        [--png-level N] [--png-filter N]\n"
      "                        [--backend cpu|opencl|cuda|vulkan]\n"
      "  ico encode-stream-jp2 <in.bmp> <out.jp2|out.j2k|out.jpf|out.jpx>\n"
      "  ico decode <in> <out.bmp> [--backend cpu|opencl|cuda|vulkan]\n"
      "  ico compare <a> <b> [--backend cpu|opencl|cuda|vulkan]\n"
      "  ico matrix <input.bmp> <output-dir>\n"
      "\n"
      "in/out 的格式由文件内容（魔数）与扩展名共同决定；BMP 只用于中转。\n");
}

bool writeFile(const std::string &path, const std::vector<uint8_t> &data,
               std::string *err);
bool loadImage(Router &router, const std::string &path, Image *out,
               std::string *err);
long long nowMicros();

uint16_t readLe16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
}

uint32_t readLe32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

struct Bmp24RowSource final {
  std::ifstream input;
  int width = 0;
  int height = 0;
  int channels = 0;
  uint64_t pixelOffset = 0;
  uint64_t stride = 0;
  bool bottomUp = false;
  std::array<uint8_t, 256> grayPalette{};

  static bool open(const std::string &path, Bmp24RowSource *out,
                   std::string *error) {
    out->input.open(path, std::ios::binary);
    std::array<uint8_t, 54> header{};
    if (!out->input.read(reinterpret_cast<char *>(header.data()), header.size())) {
      *error = "cannot read BMP header";
      return false;
    }
    const int32_t width = static_cast<int32_t>(readLe32(header.data() + 18));
    const int32_t rawHeight = static_cast<int32_t>(readLe32(header.data() + 22));
    const auto bitsPerPixel = readLe16(header.data() + 28);
    if (header[0] != 'B' || header[1] != 'M' || readLe32(header.data() + 14) < 40 ||
        width <= 0 || rawHeight == 0 || readLe16(header.data() + 26) != 1 ||
        (bitsPerPixel != 8 && bitsPerPixel != 24) || readLe32(header.data() + 30) != 0) {
      *error = "streaming only supports uncompressed 8-bit gray or 24-bit BMP";
      return false;
    }
    out->width = width;
    out->height = rawHeight > 0 ? rawHeight : -rawHeight;
    out->channels = bitsPerPixel == 8 ? 1 : 3;
    out->pixelOffset = readLe32(header.data() + 10);
    out->stride = (static_cast<uint64_t>(width) * out->channels + 3U) & ~uint64_t{3};
    out->bottomUp = rawHeight > 0;
    if (bitsPerPixel == 8) {
      if (out->pixelOffset < 54U + 256U * 4U) {
        *error = "8-bit BMP has no full grayscale palette";
        return false;
      }
      std::array<uint8_t, 256U * 4U> palette{};
      out->input.seekg(54);
      if (!out->input.read(reinterpret_cast<char *>(palette.data()), palette.size())) {
        *error = "cannot read BMP grayscale palette";
        return false;
      }
      for (size_t index = 0; index < out->grayPalette.size(); ++index) {
        const auto offset = index * 4U;
        if (palette[offset] != palette[offset + 1U] ||
            palette[offset] != palette[offset + 2U]) {
          *error = "8-bit BMP palette is not grayscale";
          return false;
        }
        out->grayPalette[index] = palette[offset];
      }
    }
    return true;
  }
};

ico_status readBmp24StreamingRow(void *user, const int y, uint8_t *destination,
                                 const size_t destinationSize) {
  auto *source = static_cast<Bmp24RowSource *>(user);
  if (source == nullptr || destination == nullptr || y < 0 || y >= source->height ||
      destinationSize != static_cast<size_t>(source->width) * source->channels) {
    return ICO_ERR_INVALID_ARGUMENT;
  }
  const int fileRow = source->bottomUp ? source->height - 1 - y : y;
  source->input.seekg(static_cast<std::streamoff>(
      source->pixelOffset + static_cast<uint64_t>(fileRow) * source->stride));
  std::vector<uint8_t> bgr(static_cast<size_t>(source->stride));
  if (!source->input.read(reinterpret_cast<char *>(bgr.data()),
                          static_cast<std::streamsize>(bgr.size()))) {
    return ICO_ERR_IO;
  }
  if (source->channels == 1) {
    for (int x = 0; x < source->width; ++x) {
      destination[x] = source->grayPalette[bgr[static_cast<size_t>(x)]];
    }
    return ICO_OK;
  }
  for (int x = 0; x < source->width; ++x) {
    const auto i = static_cast<size_t>(x) * 3U;
    destination[i] = bgr[i + 2];
    destination[i + 1] = bgr[i + 1];
    destination[i + 2] = bgr[i];
  }
  return ICO_OK;
}

int cmdEncodeStreamJpeg2000(const std::string &input, const std::string &output) {
  const auto dot = output.find_last_of('.');
  const auto extension = dot == std::string::npos ? std::string() : output.substr(dot);
  std::string lowered = extension;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lowered != ".jp2" && lowered != ".j2k" && lowered != ".jpf" && lowered != ".jpx") {
    std::fprintf(stderr, "error: streaming output must use .jp2/.j2k/.jpf/.jpx\n");
    return 1;
  }
  Bmp24RowSource source;
  std::string error;
  if (!Bmp24RowSource::open(input, &source, &error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  ico_codec *codec = nullptr;
  if (image_codec_offload_create_forced(&codec, ICO_BACKEND_CPU) != ICO_OK ||
      codec == nullptr) {
    std::fprintf(stderr, "error: cannot create CPU streaming codec\n");
    return 1;
  }
  const auto start = nowMicros();
  const auto status = image_codec_offload_encode_jpeg2000_rows(
      codec, output.c_str(), source.width, source.height, source.channels,
      lowered == ".j2k" ? ICO_JPEG2000_CONTAINER_J2K : ICO_JPEG2000_CONTAINER_JP2,
      readBmp24StreamingRow, &source);
  image_codec_offload_destroy(codec);
  if (status != ICO_OK) {
    std::fprintf(stderr, "streaming encode failed: %s\n",
                 image_codec_offload_status_string(status));
    return 1;
  }
  const auto milliseconds = static_cast<double>(nowMicros() - start) / 1000.0;
  std::printf("%dx%d x%d BMP -> JPEG 2000 CPU tile stream in %.1f ms\n",
              source.width, source.height, source.channels, milliseconds);
  return 0;
}

int cmdMatrix(const std::string &input, const std::string &output_dir) {
  Router router;
  std::string error;
  Image source;
  if (!loadImage(router, input, &source, &error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  std::error_code directory_error;
  std::filesystem::create_directories(output_dir, directory_error);
  if (directory_error) {
    std::fprintf(stderr, "error: cannot create matrix directory: %s\n",
                 directory_error.message().c_str());
    return 1;
  }
  std::ofstream manifest(std::filesystem::path(output_dir) / "manifest.csv",
                         std::ios::trunc);
  if (!manifest) {
    std::fprintf(stderr, "error: cannot create matrix manifest\n");
    return 1;
  }
  manifest << "backend,format,status,bytes,width,height,channels,elapsed_ms,error\n";

  const std::vector<BackendKind> backends = {
      BackendKind::Cpu, BackendKind::OpenCl, BackendKind::Cuda,
      BackendKind::Vulkan};
  struct MatrixFormat final {
    Format format;
    const char *name;
    const char *suffix;
    image_codec::offload::Jpeg2000Container jpeg2000Container =
        image_codec::offload::Jpeg2000Container::Jp2;
  };
  // JPEG 2000 的四种用户可选后缀不能在矩阵中折叠成一个抽象 Format：.j2k
  // 是裸 codestream，而 .jp2/.jpf/.jpx 都必须实际产出并重新解码验证。
  const std::vector<MatrixFormat> formats = {
      {Format::Jpeg, "jpeg", "jpg"},
      {Format::Tiff, "tiff", "tiff"},
      {Format::Png, "png", "png"},
      {Format::Jpeg2000, "jpeg2000-jp2", "jp2"},
      {Format::Jpeg2000, "jpeg2000-j2k", "j2k",
       image_codec::offload::Jpeg2000Container::J2k},
      {Format::Jpeg2000, "jpeg2000-jpf", "jpf"},
      {Format::Jpeg2000, "jpeg2000-jpx", "jpx"},
      {Format::JpegXl, "jpeg-xl", "jxl"},
  };
  bool hard_failure = false;
  for (const BackendKind backend : backends) {
    for (const MatrixFormat &matrix_format : formats) {
      const std::string backend_name =
          image_codec::offload::backendName(backend);
      const std::string format_name = matrix_format.name;
      const auto output = std::filesystem::path(output_dir) /
                          (backend_name + "-" + format_name + "." +
                           matrix_format.suffix);
      const long long start = nowMicros();
      EncodeParams params;
      params.jpeg2000_container = matrix_format.jpeg2000Container;
      auto encoded = router.encodeWith(backend, matrix_format.format, source,
                                       params);
      const double elapsed = static_cast<double>(nowMicros() - start) / 1000.0;
      if (!encoded) {
        const auto status = encoded.error().code;
        const std::string escaped = encoded.error().message;
        manifest << backend_name << ',' << format_name << ','
                 << (status == image_codec::offload::Status::BackendUnavailable ||
                             status == image_codec::offload::Status::UnsupportedFormat
                         ? "skipped"
                         : "failed")
                 << ",0,0,0,0," << elapsed << ",\"" << escaped << "\"\n";
        if (status != image_codec::offload::Status::BackendUnavailable &&
            status != image_codec::offload::Status::UnsupportedFormat) {
          hard_failure = true;
        }
        continue;
      }
      if (!writeFile(output.string(), *encoded, &error)) {
        manifest << backend_name << ',' << format_name
                 << ",failed,0,0,0,0," << elapsed << ",\"" << error
                 << "\"\n";
        hard_failure = true;
        continue;
      }
      auto decoded = router.decodeWith(backend, matrix_format.format,
                                       encoded->data(), encoded->size());
      if (!decoded || decoded->width != source.width ||
          decoded->height != source.height ||
          decoded->channels != source.channels) {
        const std::string message =
            decoded ? "decoded geometry/channel mismatch"
                    : decoded.error().message;
        manifest << backend_name << ',' << format_name
                 << ",failed," << encoded->size() << ",0,0,0," << elapsed
                 << ",\"" << message << "\"\n";
        hard_failure = true;
        continue;
      }
      manifest << backend_name << ',' << format_name << ",passed,"
               << encoded->size() << ',' << decoded->width << ','
               << decoded->height << ',' << decoded->channels << ',' << elapsed
               << ",\"\"\n";
      std::printf("matrix %-6s %-9s passed (%zu bytes, %.1f ms)\n",
                  backend_name.c_str(), format_name.c_str(), encoded->size(),
                  elapsed);
    }
  }
  return hard_failure ? 1 : 0;
}

bool readFile(const std::string &path, std::vector<uint8_t> *out,
              std::string *err) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    if (err != nullptr) *err = "cannot open " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(f);
    if (err != nullptr) *err = "cannot stat " + path;
    return false;
  }
  out->resize(static_cast<size_t>(size));
  const size_t read =
      out->empty() ? 0 : std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  if (read != out->size()) {
    if (err != nullptr) *err = "short read on " + path;
    return false;
  }
  return true;
}

// 只读文件开头若干字节：用于「先看魔数/BMP 头再决定要不要整套读进来」。
// Merge.bmp 是 1.9 GB，`ico info` 不该为了打印两行尺寸把它全读进内存。
bool readPrefix(const std::string &path, size_t max_bytes,
                std::vector<uint8_t> *out, std::string *err) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    if (err != nullptr) *err = "cannot open " + path;
    return false;
  }
  out->resize(max_bytes);
  const size_t read = std::fread(out->data(), 1, max_bytes, f);
  std::fclose(f);
  out->resize(read);
  return true;
}

bool writeFile(const std::string &path, const std::vector<uint8_t> &data,
               std::string *err) {
  std::FILE *f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    if (err != nullptr) *err = "cannot create " + path;
    return false;
  }
  const size_t written =
      data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
  if (written != data.size()) {
    if (err != nullptr) *err = "short write on " + path;
    return false;
  }
  return true;
}

bool hasExtension(const std::string &path, const char *ext) {
  if (path.size() < std::strlen(ext)) return false;
  std::string tail = path.substr(path.size() - std::strlen(ext));
  for (char &c : tail) c = static_cast<char>(std::tolower(c));
  return tail == ext;
}

bool isBmp(const std::vector<uint8_t> &data) {
  return data.size() >= 2 && data[0] == 'B' && data[1] == 'M';
}

Format formatFromPathOrBytes(const std::string &path,
                             const std::vector<uint8_t> &data) {
  const Format probed =
      image_codec::offload::probeFormat(data.data(), data.size());
  if (probed != Format::Unknown) return probed;

  const std::size_t dot = path.find_last_of('.');
  if (dot != std::string::npos) {
    return image_codec::offload::formatFromExtension(path.substr(dot));
  }
  return Format::Unknown;
}

bool parseBackend(const std::string &value, BackendKind *out) {
  if (value == "cpu") {
    *out = BackendKind::Cpu;
    return true;
  }
  if (value == "opencl") {
    *out = BackendKind::OpenCl;
    return true;
  }
  if (value == "cuda") {
    *out = BackendKind::Cuda;
    return true;
  }
  if (value == "vulkan") {
    *out = BackendKind::Vulkan;
    return true;
  }
  return false;
}

// 读一张图：BMP 走中转格式，其它走路由层解码。
bool loadImage(Router &router, const std::string &path, Image *out,
               std::string *err) {
  std::vector<uint8_t> bytes;
  if (!readFile(path, &bytes, err)) return false;

  if (isBmp(bytes)) {
    auto bmp =
        image_codec::offload::common::readBmp(bytes.data(), bytes.size());
    if (!bmp) {
      if (err != nullptr) *err = "bmp: " + bmp.error().message;
      return false;
    }
    *out = std::move(*bmp);
    return true;
  }

  const Format format = formatFromPathOrBytes(path, bytes);
  if (format == Format::Unknown) {
    if (err != nullptr) *err = "unknown container format: " + path;
    return false;
  }

  const auto decoded = router.decode(format, bytes.data(), bytes.size());
  if (!decoded) {
    if (err != nullptr) *err = decoded.error().message;
    return false;
  }
  *out = std::move(*decoded);
  return true;
}

long long nowMicros() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::microseconds>(
             clock::now().time_since_epoch())
      .count();
}

void printOutcome(const image_codec::offload::Outcome &outcome) {
  std::printf("backend: %s", image_codec::offload::backendName(outcome.used));
  if (outcome.fellBack()) {
    std::printf("  (fell back)");
  }
  std::printf("\n");
  for (const std::string &note : outcome.notes) {
    std::printf("  - %s\n", note.c_str());
  }
}

// ---- 子命令 ----

int cmdBackends() {
  image_codec::offload::Router router;
  const auto probed = router.probeBackends();

  std::printf("%-8s %-8s %s\n", "backend", "state", "detail");
  for (const auto &[kind, detail] : probed) {
    const bool ready = detail == "ready";
    std::printf("%-8s %-8s %s\n", image_codec::offload::backendName(kind),
                ready ? "ready" : "unusable", detail.c_str());
  }
  return 0;
}

int cmdInfo(const std::string &path) {
  std::vector<uint8_t> head;
  std::string err;
  if (!readPrefix(path, 65536, &head, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  std::printf("file   : %s\n", path.c_str());

  // BMP 是 CLI/测试的中转格式：只读头就能报尺寸，不必把 1.9 GB 读进来。
  if (isBmp(head)) {
    const auto header =
        image_codec::offload::common::probeBmp(head.data(), head.size());
    if (!header) {
      std::fprintf(stderr, "error: %s\n", header.error().message.c_str());
      return 1;
    }
    std::printf("format : bmp (CLI 中转格式)\n");
    std::printf("size   : %dx%d x %d channel(s)\n", header->width,
                header->height, header->channels);
    return 0;
  }

  const Format format = formatFromPathOrBytes(path, head);
  std::printf("format : %s\n", image_codec::offload::formatName(format));
  if (format == Format::Unknown) return 0;

  std::vector<uint8_t> bytes;
  if (!readFile(path, &bytes, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  std::printf("bytes  : %zu\n", bytes.size());

  image_codec::offload::Router router;
  const auto decoded = router.decode(format, bytes.data(), bytes.size());
  if (!decoded) {
    std::fprintf(stderr, "decode failed: %s\n",
                 decoded.error().message.c_str());
    return 1;
  }
  std::printf("size   : %dx%d x %d channel(s)\n", decoded->width,
              decoded->height, decoded->channels);
  return 0;
}

int cmdEncode(const std::string &in, const std::string &out,
              const EncodeParams &params, bool force, BackendKind kind) {
  image_codec::offload::Router router;
  std::string err;

  Image image;
  if (!loadImage(router, in, &image, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  const std::size_t dot = out.find_last_of('.');
  const Format format =
      dot == std::string::npos
          ? Format::Unknown
          : image_codec::offload::formatFromExtension(out.substr(dot));
  if (format == Format::Unknown) {
    std::fprintf(stderr, "error: unknown output format: %s\n", out.c_str());
    return 1;
  }

  // JPEG 2000 的 .j2k 是裸码流；.jp2/.jpf/.jpx 属于 JP2 容器家族。
  EncodeParams effective_params = params;
  if (format == Format::Jpeg2000 && dot != std::string::npos) {
    std::string extension = out.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                   });
    if (extension == ".j2k") {
      effective_params.jpeg2000_container =
          image_codec::offload::Jpeg2000Container::J2k;
    }
  }

  const long long t0 = nowMicros();
  image_codec::offload::Outcome outcome;
  auto encoded = force ? router.encodeWith(kind, format, image, effective_params)
                       : router.encode(format, image, effective_params, &outcome);
  const long long t1 = nowMicros();

  if (!encoded) {
    std::fprintf(stderr, "encode failed: %s (%s)\n",
                 encoded.error().message.c_str(),
                 image_codec::offload::statusName(encoded.error().code));
    return 1;
  }

  if (!writeFile(out, *encoded, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  const double ms = static_cast<double>(t1 - t0) / 1000.0;
  const double mib =
      static_cast<double>(image.pixels.size()) / (1024.0 * 1024.0);
  std::printf("%dx%d x%d -> %s: %zu bytes in %.1f ms (%.1f MiB/s)\n",
              image.width, image.height, image.channels,
              image_codec::offload::formatName(format), encoded->size(), ms,
              ms > 0.0 ? mib / (ms / 1000.0) : 0.0);
  if (force) {
    std::printf("backend: %s (forced)\n",
                image_codec::offload::backendName(kind));
  } else {
    printOutcome(outcome);
  }
  return 0;
}

int cmdDecode(const std::string &in, const std::string &out, bool force,
              BackendKind kind) {
  image_codec::offload::Router router;
  std::string err;

  std::vector<uint8_t> bytes;
  if (!readFile(in, &bytes, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  const Format format = formatFromPathOrBytes(in, bytes);
  if (format == Format::Unknown) {
    std::fprintf(stderr, "error: unknown container format: %s\n", in.c_str());
    return 1;
  }

  const long long t0 = nowMicros();
  image_codec::offload::Outcome outcome;
  auto decoded =
      force ? router.decodeWith(kind, format, bytes.data(), bytes.size())
            : router.decode(format, bytes.data(), bytes.size(), &outcome);
  const long long t1 = nowMicros();

  if (!decoded) {
    std::fprintf(stderr, "decode failed: %s (%s)\n",
                 decoded.error().message.c_str(),
                 image_codec::offload::statusName(decoded.error().code));
    return 1;
  }

  auto bmp = image_codec::offload::common::writeBmp(*decoded);
  if (!bmp) {
    std::fprintf(stderr, "error: %s\n", bmp.error().message.c_str());
    return 1;
  }
  if (!writeFile(out, *bmp, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  const double ms = static_cast<double>(t1 - t0) / 1000.0;
  std::printf("%s -> %dx%d x%d BMP in %.1f ms\n",
              image_codec::offload::formatName(format), decoded->width,
              decoded->height, decoded->channels, ms);
  if (force) {
    std::printf("backend: %s (forced)\n",
                image_codec::offload::backendName(kind));
  } else {
    printOutcome(outcome);
  }
  return 0;
}

int cmdCompare(const std::string &a_path, const std::string &b_path, bool force,
               BackendKind kind) {
  image_codec::offload::Router router;
  std::string err;

  Image a;
  Image b;
  if (!loadImage(router, a_path, &a, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  if (!loadImage(router, b_path, &b, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  // 逐后端把数字并排摆出来：自动模式下这就是「换后端到底换没换数字」的证据。
  std::printf("%-8s %14s %14s %14s %14s\n", "backend", "mse", "psnr", "ssim",
              "ms-ssim");

  int compared = 0;
  for (BackendKind candidate : router.order()) {
    if (force && candidate != kind) continue;

    auto mse = router.mseWith(candidate, a, b);
    if (!mse) {
      std::printf("%-8s %s\n", image_codec::offload::backendName(candidate),
                  mse.error().message.c_str());
      continue;
    }
    auto psnr = router.psnrWith(candidate, a, b);
    auto ssim = router.ssimWith(candidate, a, b);
    if (!psnr || !ssim) {
      std::printf("%-8s %s\n", image_codec::offload::backendName(candidate),
                  "metric failed");
      continue;
    }

    std::printf("%-8s %14.9f %14.9f %14.9f %14.9f\n",
                image_codec::offload::backendName(candidate), *mse, *psnr,
                ssim->ssim, ssim->ms_ssim);
    ++compared;
  }

  if (compared == 0) {
    std::fprintf(stderr, "no backend produced a result\n");
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    printUsage();
    return 1;
  }

  const std::string command = argv[1];

  // 默认值直接取 C++ 侧的 EncodeParams；CLI 只覆盖用户显式给出的项。
  EncodeParams params;
  params.png_filter = -1;

  bool force = false;
  BackendKind forced_kind = BackendKind::Cpu;

  std::vector<std::string> positional;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&](const char *what) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a value\n", what);
        std::exit(1);
      }
      return argv[++i];
    };

    if (arg == "--backend") {
      const std::string value = next("--backend");
      if (!parseBackend(value, &forced_kind)) {
        std::fprintf(stderr, "error: unknown backend: %s\n", value.c_str());
        return 1;
      }
      force = true;
    } else if (arg == "--quality") {
      params.jpeg_quality = std::atoi(next("--quality"));
    } else if (arg == "--subsampling") {
      params.jpeg_subsampling = std::atoi(next("--subsampling"));
    } else if (arg == "--compression") {
      params.tiff_compression = std::atoi(next("--compression"));
    } else if (arg == "--tile") {
      params.tiff_tile_size = std::atoi(next("--tile"));
    } else if (arg == "--rows") {
      params.tiff_rows_per_strip = std::atoi(next("--rows"));
    } else if (arg == "--png-level") {
      params.png_compression = std::atoi(next("--png-level"));
    } else if (arg == "--png-filter") {
      params.png_filter = std::atoi(next("--png-filter"));
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      return 0;
    } else {
      positional.push_back(arg);
    }
  }

  if (command == "backends") return cmdBackends();

  if (command == "info") {
    if (positional.size() != 1) {
      printUsage();
      return 1;
    }
    return cmdInfo(positional[0]);
  }

  if (command == "encode") {
    if (positional.size() != 2) {
      printUsage();
      return 1;
    }
    return cmdEncode(positional[0], positional[1], params, force, forced_kind);
  }

  if (command == "encode-stream-jp2") {
    if (positional.size() != 2 || force) {
      printUsage();
      return 1;
    }
    return cmdEncodeStreamJpeg2000(positional[0], positional[1]);
  }

  if (command == "decode") {
    if (positional.size() != 2) {
      printUsage();
      return 1;
    }
    return cmdDecode(positional[0], positional[1], force, forced_kind);
  }

  if (command == "compare") {
    if (positional.size() != 2) {
      printUsage();
      return 1;
    }
    return cmdCompare(positional[0], positional[1], force, forced_kind);
  }

  if (command == "matrix") {
    if (positional.size() != 2 || force) {
      printUsage();
      return 1;
    }
    return cmdMatrix(positional[0], positional[1]);
  }

  std::fprintf(stderr, "error: unknown command: %s\n", command.c_str());
  printUsage();
  return 1;
}
