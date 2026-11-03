/*
 * File: main.cpp
 * Brief: Main.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// cli/main.cpp - OpenCL-Image-Codec 测试 CLI（单可执行 + 子命令）。
//
// 集成各模块测试入口（C linkage，见各 *_test.h）：
//   - JPEG : image_codec_jpeg_info / image_codec_jpeg_decode / image_codec_jpeg_encode
//   - TIFF : image_codec_tiff_info / image_codec_tiff_decode / image_codec_tiff_encode
//   - PNG  : image_codec_png_info / image_codec_png_decode / image_codec_png_encode
#include "jpeg_test.h"
#include "ocl_device.h"
#include "image_codec_platform.h"
#include "png_test.h"
#include "tiff_test.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace image_codec;

namespace {

enum class FileFormat { Unknown, Jpeg, Tiff, Png };

FileFormat detect_format(const char *path) {
  FILE *f = std::fopen(path, "rb");
  if (f == nullptr) {
    return FileFormat::Unknown;
  }
  unsigned char head[8] = {0};
  const size_t n = std::fread(head, 1, sizeof(head), f);
  std::fclose(f);
  if (n >= 8 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' &&
      head[3] == 'G') {
    return FileFormat::Png;
  }
  if (n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) {
    return FileFormat::Jpeg;
  }
  if (n >= 4 && ((head[0] == 0x49 && head[1] == 0x49 && head[2] == 0x2A &&
                  head[3] == 0x00) ||
                 (head[0] == 0x4D && head[1] == 0x4D && head[2] == 0x00 &&
                  head[3] == 0x2A))) {
    return FileFormat::Tiff;
  }
  return FileFormat::Unknown;
}

const char *format_name(FileFormat f) {
  switch (f) {
  case FileFormat::Jpeg:
    return "JPEG";
  case FileFormat::Tiff:
    return "TIFF";
  case FileFormat::Png:
    return "PNG";
  default:
    return "unknown";
  }
}

// --backend opencl|cpu -> code（opencl=0, cpu=2）。
// 返回 0 表示解析成功。
int backend_code(const char *name, int *code) {
  if (name == nullptr || *name == '\0') {
    *code = 0;
    return 0;
  }
  if (std::strcmp(name, "opencl") == 0) {
    *code = 0;
    return 0;
  }
  if (std::strcmp(name, "cpu") == 0) {
    *code = 2;
    return 0;
  }
  return -1;
}

const char *find_opt(int argc, char **argv, const char *key) {
  for (int i = 0; i < argc - 1; ++i) {
    if (std::strcmp(argv[i], key) == 0) {
      return argv[i + 1];
    }
  }
  return nullptr;
}

int find_opt_int(int argc, char **argv, const char *key, int def) {
  const char *v = find_opt(argc, argv, key);
  return v != nullptr ? std::atoi(v) : def;
}

void print_usage() {
  std::printf(
      "OpenCL-Image-Codec CLI\n"
      "Usage: image_codec <command> [options]\n"
      "Commands:\n"
      "  list-devices                                Enumerate OpenCL devices\n"
      "  info <in>                                   Print file header info "
      "(jpeg/tiff/png)\n"
      "  decode <in> <out> [--backend opencl|cpu]\n"
      "  encode <in> <out> [--backend opencl|cpu]\n"
      "            [--jpeg-quality N] [--jpeg-subsampling 444|422|420]\n"
      "            [--tiff-compression 1|5] [--rows-per-strip N]\n"
      "            [--png-level 0..9] [--png-filter -1|0..4]\n"
      "  bench <in> [--iterations N] [--backend opencl|cpu]\n");
}

int cmd_list_devices(int, char **) {
  const std::vector<OclDeviceInfo> devs = OclDevice::enumerate();
  if (devs.empty()) {
    std::fprintf(stderr, "No OpenCL platforms/devices found.\n");
    return 1;
  }
  std::printf("%zu OpenCL device(s):\n", devs.size());
  for (size_t i = 0; i < devs.size(); ++i) {
    const OclDeviceInfo &d = devs[i];
    std::printf("[%zu] %s | %s | %s | CL %u.%u\n", i, d.deviceName.c_str(),
                d.vendor.c_str(), d.clVersion.c_str(), d.major, d.minor);
  }
  return 0;
}

int cmd_info(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "info: missing input file\n");
    return 2;
  }
  const char *in = argv[1];
  const FileFormat fmt = detect_format(in);
  if (fmt == FileFormat::Jpeg) {
    return image_codec_jpeg_info(in);
  }
  if (fmt == FileFormat::Tiff) {
    return image_codec_tiff_info(in);
  }
  if (fmt == FileFormat::Png) {
    return image_codec_png_info(in);
  }
  std::fprintf(stderr, "info: unrecognized format (need JPEG/TIFF/PNG): %s\n",
               in);
  return 2;
}

int cmd_decode(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "decode: usage decode <in> <out> [--backend ...]\n");
    return 2;
  }
  const char *in = argv[1];
  const char *out = argv[2];
  const char *backend_name = find_opt(argc, argv, "--backend");
  int backend = 0;
  if (backend_code(backend_name, &backend) != 0) {
    std::fprintf(stderr, "decode: unknown backend '%s'\n", backend_name);
    return 2;
  }
  const FileFormat fmt = detect_format(in);
  switch (fmt) {
  case FileFormat::Jpeg:
    return image_codec_jpeg_decode(in, out, backend == 2 ? 1 : 0);
  case FileFormat::Tiff:
    return image_codec_tiff_decode(in, out, backend);
  case FileFormat::Png:
    return image_codec_png_decode(in, out, backend);
  default:
    std::fprintf(stderr, "decode: unrecognized format: %s\n", in);
    return 2;
  }
}

// 按输出扩展名选择编码格式。
FileFormat out_format(const char *out) {
  const char *dot = std::strrchr(out, '.');
  if (dot == nullptr) {
    return FileFormat::Unknown;
  }
  if (std::strcmp(dot, ".jpg") == 0 || std::strcmp(dot, ".jpeg") == 0) {
    return FileFormat::Jpeg;
  }
  if (std::strcmp(dot, ".tif") == 0 || std::strcmp(dot, ".tiff") == 0) {
    return FileFormat::Tiff;
  }
  if (std::strcmp(dot, ".png") == 0) {
    return FileFormat::Png;
  }
  return FileFormat::Unknown;
}

int cmd_encode(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "encode: usage encode <in> <out> [--backend ...]\n");
    return 2;
  }
  const char *in = argv[1];
  const char *out = argv[2];
  const char *backend_name = find_opt(argc, argv, "--backend");
  int backend = 0;
  if (backend_code(backend_name, &backend) != 0) {
    std::fprintf(stderr, "encode: unknown backend '%s'\n", backend_name);
    return 2;
  }
  const FileFormat fmt = out_format(out);
  switch (fmt) {
  case FileFormat::Jpeg: {
    const int quality = find_opt_int(argc, argv, "--jpeg-quality", 85);
    const int subsampling = find_opt_int(argc, argv, "--jpeg-subsampling", 0);
    return image_codec_jpeg_encode(in, out, backend == 2 ? 1 : 0, quality,
                                   subsampling);
  }
  case FileFormat::Tiff: {
    const int compression = find_opt_int(argc, argv, "--tiff-compression", 5);
    const int rows_per_strip = find_opt_int(argc, argv, "--rows-per-strip", 0);
    return image_codec_tiff_encode(in, out, backend, compression,
                                   rows_per_strip);
  }
  case FileFormat::Png: {
    const int level = find_opt_int(argc, argv, "--png-level", 6);
    const int filter = find_opt_int(argc, argv, "--png-filter", -1);
    return image_codec_png_encode(in, out, backend, level, filter);
  }
  default:
    std::fprintf(stderr, "encode: unsupported output extension "
                         "(use .jpg/.jpeg/.tif/.tiff/.png)\n");
    return 2;
  }
}

int cmd_bench(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "bench: usage bench <in> [--iterations N] [--backend ...]\n");
    return 2;
  }
  const char *in = argv[1];
  const int iterations = find_opt_int(argc, argv, "--iterations", 5);
  const char *backend_name = find_opt(argc, argv, "--backend");
  int backend = 0;
  if (backend_code(backend_name, &backend) != 0) {
    std::fprintf(stderr, "bench: unknown backend '%s'\n", backend_name);
    return 2;
  }
  const FileFormat fmt = detect_format(in);
  if (fmt == FileFormat::Unknown) {
    std::fprintf(stderr, "bench: unrecognized format: %s\n", in);
    return 2;
  }
  // 解码计时（输出写到临时路径；各模块忽略重复写同一文件的代价差异不大）。
  const char *tmp_out = "bench_out.bmp";
  const long long t0 = image_codec_now_ns();
  for (int i = 0; i < iterations; ++i) {
    int rc = 0;
    if (fmt == FileFormat::Jpeg) {
      rc = image_codec_jpeg_decode(in, tmp_out, backend == 2 ? 1 : 0);
    } else if (fmt == FileFormat::Png) {
      rc = image_codec_png_decode(in, tmp_out, backend);
    } else {
      rc = image_codec_tiff_decode(in, tmp_out, backend);
    }
    if (rc != 0) {
      std::fprintf(stderr, "bench: decode failed at iteration %d (rc=%d)\n", i,
                   rc);
      return 1;
    }
  }
  const long long dt = image_codec_now_ns() - t0;
  const double per =
      static_cast<double>(dt) / static_cast<double>(iterations) / 1e6;
  std::printf(
      "bench: %s -> %d decodes in %.3f ms total, %.3f ms/decode (%.1f fps)\n",
      in, iterations, static_cast<double>(dt) / 1e6, per,
      1000.0 / (per > 0 ? per : 1e-9));
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage();
    return 2;
  }
  const std::string cmd = argv[1];
  int rc = 2;
  if (cmd == "list-devices") {
    rc = cmd_list_devices(argc - 1, argv + 1);
  } else if (cmd == "info") {
    rc = cmd_info(argc - 1, argv + 1);
  } else if (cmd == "decode") {
    rc = cmd_decode(argc - 1, argv + 1);
  } else if (cmd == "encode") {
    rc = cmd_encode(argc - 1, argv + 1);
  } else if (cmd == "bench") {
    rc = cmd_bench(argc - 1, argv + 1);
  } else {
    print_usage();
  }
  return rc;
}
