/*
 * File: icp_selftest.cpp
 * Brief: Icp selftest.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// icp_selftest.cpp - Image.Codec.Platform.Intel 的自测 CLI。
//
// 子命令：
//   units                              打印 oneVPL 硬件单元探测结果（含不可用原因）
//   encode <in.bmp> <out.jpg> [q]      Intel 媒体引擎硬件 JPEG 编码
//   decode <in.jpg> <out.bmp>          Intel 媒体引擎硬件 JPEG 解码
//   tiff <in.bmp> <out.tiff> [选项]    RGB -> TIFF（硬件 JPEG strips 或未压缩）
//   selftest [--require-hardware]      无参数默认命令：容器自测 + 硬件往返
//
// 之所以把 TIFF 容器自测做成「默认就跑」：没有 Intel 媒体引擎的机器
// （本仓库的 CI 就是）也应该能验证容器代码，而不是所有测试都 SKIP。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "bmp24.h"
#include "jpeg_stream.h"
#include "onevpl_test.h"
#include "selftest_support.h"
#include "tiff_jpeg_strip_writer.h"
#include "vpl_device.h"
#include "vpl_jpeg_decoder.h"
#include "vpl_jpeg_encoder.h"

namespace {

using oic::jpeg::onevpl::VplJpegDecoder;
using oic::jpeg::onevpl::VplJpegEncoder;
using oic::jpeg::onevpl::VplProbeResult;
using oic::jpeg::onevpl::VplUnitInfo;

constexpr uint32_t kDefaultQuality = 85;
constexpr uint32_t kDefaultRowsPerStrip = 32;

void print_usage() {
  std::printf(
      "用法：\n"
      "  icp_selftest                            探测硬件单元并跑自测\n"
      "  icp_selftest units                      只打印硬件单元\n"
      "  icp_selftest encode <in.bmp> <out.jpg> [quality]\n"
      "  icp_selftest decode <in.jpg> <out.bmp>\n"
      "  icp_selftest tiff <in.bmp> <out.tiff> [--rows N] [--quality Q]\n"
      "                                          [--none] [--software]\n"
      "  icp_selftest selftest [--require-hardware]\n");
}

// BMP 里是 BGR，编码入口要 RGB。
std::vector<uint8_t> bgr_to_rgb(const std::vector<uint8_t> &bgr) {
  std::vector<uint8_t> rgb(bgr.size(), 0);
  for (size_t i = 0; i + 2 < bgr.size(); i += 3) {
    rgb[i + 0] = bgr[i + 2];
    rgb[i + 1] = bgr[i + 1];
    rgb[i + 2] = bgr[i + 0];
  }
  return rgb;
}

bool load_bmp_rgb(const char *path, uint32_t &width, uint32_t &height,
                  std::vector<uint8_t> &rgb, std::string &error) {
  std::vector<uint8_t> bgr;
  if (!oic::jpeg::onevpl::bmp_load_24(path, width, height, bgr, error)) {
    return false;
  }
  rgb = bgr_to_rgb(bgr);
  return true;
}

int command_units() {
  std::printf("%s\n", oic::jpeg::onevpl::vpl_probe().describe().c_str());
  return 0;
}

// ---- TIFF：用硬件 JPEG strip 编码器写 TIFF ----
struct HardwareStripEncoder {
  uint32_t quality = kDefaultQuality;
  std::unique_ptr<VplJpegEncoder> encoder;
  uint32_t encoded_width = 0;
  uint32_t encoded_rows = 0;
  std::string error;

  bool operator()(const uint8_t *rgb, uint32_t width, uint32_t rows,
                  std::vector<uint8_t> &jpeg) {
    // 末条 strip 比其它条矮，硬件编码器按「固定宽高」建会话，因此尺寸
    // 变了就要重建（同一质量下量化/霍夫曼表不变，JPEGTables 仍可共用）。
    if (!encoder || width != encoded_width || rows != encoded_rows) {
      if (encoder) {
        encoder->close();
        encoder.reset();
      }
      encoder = std::make_unique<VplJpegEncoder>();
      if (!encoder->init(width, rows, quality)) {
        error = encoder->last_error;
        return false;
      }
      encoded_width = width;
      encoded_rows = rows;
    }
    if (!encoder->encode_rgb24(rgb, static_cast<size_t>(width) * rows * 3,
                               jpeg)) {
      error = encoder->last_error;
      return false;
    }
    return true;
  }
};

int command_tiff(int argc, char **argv) {
  if (argc < 4) {
    print_usage();
    return 2;
  }
  const char *input = argv[2];
  const char *output = argv[3];
  uint32_t rows_per_strip = kDefaultRowsPerStrip;
  uint32_t quality = kDefaultQuality;
  bool uncompressed = false;
  bool software = false;
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--rows" && i + 1 < argc) {
      rows_per_strip = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (arg == "--quality" && i + 1 < argc) {
      quality = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (arg == "--none") {
      uncompressed = true;
    } else if (arg == "--software") {
      software = true;
    } else {
      std::fprintf(stderr, "未知参数：%s\n", arg.c_str());
      return 2;
    }
  }

  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgb;
  std::string error;
  if (!load_bmp_rgb(input, width, height, rgb, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  if (uncompressed) {
    if (!oic::tiff::write_tiff_rgb_none(output, rgb.data(), width, height,
                                        rows_per_strip, error)) {
      std::fprintf(stderr, "TIFF 写出失败：%s\n", error.c_str());
      return 1;
    }
    std::printf("TIFF（未压缩）：%ux%u rows_per_strip=%u -> %s\n", width,
                height, rows_per_strip, output);
    return 0;
  }

  oic::tiff::JpegStripEncoder encoder;
  HardwareStripEncoder hardware;
  hardware.quality = quality;
  if (software) {
    if (!oic::selftest::have_jpeg_encoder()) {
      std::fprintf(stderr, "构建时没有找到 libjpeg，无法使用 --software\n");
      return 1;
    }
    encoder.quality = quality;
    encoder.encode = [quality](const uint8_t *strip_rgb, uint32_t strip_width,
                               uint32_t strip_rows, std::vector<uint8_t> &jpeg,
                               std::string &strip_error) {
      return oic::selftest::encode_jpeg_reference(
          strip_rgb, strip_width, strip_rows, quality, jpeg, strip_error);
    };
  } else {
    encoder.quality = quality;
    encoder.encode = [&hardware](const uint8_t *strip_rgb, uint32_t strip_width,
                                 uint32_t strip_rows,
                                 std::vector<uint8_t> &jpeg,
                                 std::string &strip_error) {
      const bool ok = hardware(strip_rgb, strip_width, strip_rows, jpeg);
      if (!ok) {
        strip_error = hardware.error;
      }
      return ok;
    };
  }

  oic::tiff::JpegStripTiffReport report;
  if (!oic::tiff::write_tiff_jpeg_strips(output, rgb.data(), width, height,
                                         rows_per_strip, encoder, report,
                                         error)) {
    std::fprintf(stderr, "TIFF 写出失败：%s\n", error.c_str());
    return 1;
  }
  std::printf("TIFF（JPEG strips）：%ux%u，%u 条，rows/strip=%u，"
              "采样=%ux%u，JPEGTables=%s -> %s\n",
              width, height, report.strips, report.rows_per_strip,
              report.subsampling_h, report.subsampling_v,
              report.used_jpeg_tables ? "yes" : "no", output);
  return 0;
}

// ---- 自测 ----

// 用软件参考编码器验证 TIFF 容器（不依赖任何硬件）。
// 返回 true 表示通过或明确跳过（没有 libjpeg/libtiff）。
bool selftest_tiff_container(bool &skipped) {
  skipped = false;
  if (!oic::selftest::have_jpeg_encoder() ||
      !oic::selftest::have_tiff_decoder()) {
    std::printf("[TIFF] 跳过：缺少 libjpeg 或 libtiff（容器逻辑无法验证）\n");
    skipped = true;
    return true;
  }

  // 奇数宽高 + 32 行条带：正好覆盖「宽度补齐到偶数」「最后一条 strip 不足
  // 整条」两种边界。
  const uint32_t width = 257;
  const uint32_t height = 193;
  const uint32_t rows = 32;
  const uint32_t quality = 90;
  const std::vector<uint8_t> rgb =
      oic::selftest::make_test_image(width, height);

  // 1) 缩写码流 + JPEGTables（libtiff 写出来的形态）。
  {
    oic::tiff::JpegStripEncoder encoder;
    encoder.quality = quality;
    encoder.encode = [quality](const uint8_t *strip_rgb, uint32_t strip_width,
                               uint32_t strip_rows, std::vector<uint8_t> &jpeg,
                               std::string &error) {
      return oic::selftest::encode_jpeg_reference(
          strip_rgb, strip_width, strip_rows, quality, jpeg, error);
    };
    oic::tiff::JpegStripTiffReport report;
    std::string error;
    std::vector<uint8_t> tiff;
    oic::tiff::TiffStripImage image;
    // 先写文件再读回来，验证文件路径与容器一起工作。
    const std::string path = "icp_selftest_jpeg_strips.tiff";
    if (!oic::tiff::write_tiff_jpeg_strips(path.c_str(), rgb.data(), width,
                                           height, rows, encoder, report,
                                           error)) {
      std::printf("[TIFF] 失败：%s\n", error.c_str());
      return false;
    }
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
      std::printf("[TIFF] 失败：写出的文件打不开\n");
      return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    tiff.resize(static_cast<size_t>(size));
    const bool read_ok =
        std::fread(tiff.data(), 1, tiff.size(), file) == tiff.size();
    std::fclose(file);
    std::remove(path.c_str());
    if (!read_ok) {
      std::printf("[TIFF] 失败：读回写出的文件失败\n");
      return false;
    }

    uint32_t decoded_w = 0;
    uint32_t decoded_h = 0;
    std::vector<uint8_t> decoded;
    if (!oic::selftest::decode_tiff_reference(tiff, decoded_w, decoded_h,
                                              decoded, error)) {
      std::printf("[TIFF] libtiff 解码失败：%s\n", error.c_str());
      return false;
    }
    if (decoded_w != width || decoded_h != height) {
      std::printf("[TIFF] 失败：尺寸 %ux%u != %ux%u\n", decoded_w, decoded_h,
                  width, height);
      return false;
    }
    const double psnr =
        oic::selftest::psnr_rgb(rgb.data(), decoded.data(), rgb.size());
    std::printf("[TIFF] JPEG strips（%u 条，采样 %ux%u，JPEGTables=%s）"
                "libtiff 解码 PSNR=%.2f dB\n",
                report.strips, report.subsampling_h, report.subsampling_v,
                report.used_jpeg_tables ? "yes" : "no", psnr);
    if (psnr < 25.0) {
      std::printf("[TIFF] 失败：PSNR 过低\n");
      return false;
    }
  }

  // 2) 完整码流（不写 JPEGTables）——用于确认我们的「表拆分」不是唯一
  //    能读的形态，也确认 libtiff 读不了时能给出明确结论而不是猜。
  {
    oic::tiff::TiffStripImage image;
    image.width = width;
    image.height = height;
    image.rows_per_strip = rows;
    image.compression = oic::tiff::TiffCompression::kJpeg;
    image.photometric = 6;
    // 采样因子只在第一条 strip 上从 SOF0 读回来：后面几条的 4:4:4
    // 与 4:2:0 都合法，但 TIFF 只能写一个 YCbCrSubSampling，用「等于 1」
    // 判断首条会把真正的 4:4:4 误判成「还没设过」。
    bool first_strip = true;
    for (uint32_t y = 0; y < height; y += rows) {
      const uint32_t rows_here = std::min(rows, height - y);
      std::vector<uint8_t> strip(static_cast<size_t>(width) * rows_here * 3, 0);
      for (uint32_t row = 0; row < rows_here; ++row) {
        std::memcpy(strip.data() + static_cast<size_t>(row) * width * 3,
                    rgb.data() + static_cast<size_t>(y + row) * width * 3,
                    static_cast<size_t>(width) * 3);
      }
      std::vector<uint8_t> jpeg;
      std::string error;
      if (!oic::selftest::encode_jpeg_reference(strip.data(), width, rows_here,
                                                quality, jpeg, error)) {
        std::printf("[TIFF] 失败：%s\n", error.c_str());
        return false;
      }
      oic::tiff::JpegStreamInfo info;
      if (!oic::tiff::parse_jpeg_stream(jpeg.data(), jpeg.size(), info,
                                        error)) {
        std::printf("[TIFF] 失败：%s\n", error.c_str());
        return false;
      }
      if (first_strip) {
        image.subsampling_h = info.components[0].sampling_h;
        image.subsampling_v = info.components[0].sampling_v;
        first_strip = false;
      }
      image.strips.push_back(std::move(jpeg));
    }
    std::vector<uint8_t> tiff;
    std::string error;
    uint32_t decoded_w = 0;
    uint32_t decoded_h = 0;
    std::vector<uint8_t> decoded;
    if (!oic::tiff::encode_tiff_strips(image, tiff, error)) {
      std::printf("[TIFF] 失败（完整码流）：%s\n", error.c_str());
      return false;
    }
    if (!oic::selftest::decode_tiff_reference(tiff, decoded_w, decoded_h,
                                              decoded, error)) {
      std::printf("[TIFF] 完整码流形态 libtiff 无法解码：%s\n", error.c_str());
      return false;
    }
    const double psnr =
        oic::selftest::psnr_rgb(rgb.data(), decoded.data(), rgb.size());
    std::printf("[TIFF] 完整码流（无 JPEGTables）libtiff 解码 PSNR=%.2f dB\n",
                psnr);
    if (psnr < 25.0) {
      std::printf("[TIFF] 失败：完整码流形态 PSNR 过低\n");
      return false;
    }
  }

  // 3) 未压缩基线：必须逐字节一致。
  {
    const std::string path = "icp_selftest_none.tiff";
    std::string error;
    if (!oic::tiff::write_tiff_rgb_none(path.c_str(), rgb.data(), width, height,
                                        rows, error)) {
      std::printf("[TIFF] 失败（未压缩）：%s\n", error.c_str());
      return false;
    }
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
      std::printf("[TIFF] 失败（未压缩）：文件打不开\n");
      return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<uint8_t> tiff(static_cast<size_t>(size));
    const bool ok =
        std::fread(tiff.data(), 1, tiff.size(), file) == tiff.size();
    std::fclose(file);
    std::remove(path.c_str());
    if (!ok) {
      std::printf("[TIFF] 失败（未压缩）：读回失败\n");
      return false;
    }
    uint32_t decoded_w = 0;
    uint32_t decoded_h = 0;
    std::vector<uint8_t> decoded;
    if (!oic::selftest::decode_tiff_reference(tiff, decoded_w, decoded_h,
                                              decoded, error)) {
      std::printf("[TIFF] 未压缩形态 libtiff 解码失败：%s\n", error.c_str());
      return false;
    }
    if (decoded_w != width || decoded_h != height || decoded != rgb) {
      std::printf("[TIFF] 失败（未压缩）：像素不一致\n");
      return false;
    }
    std::printf("[TIFF] 未压缩（Compression=1）逐字节一致\n");
  }
  return true;
}

// 硬件 JPEG 往返（编码 -> 解码 -> PSNR）。
bool selftest_hardware_jpeg(const VplProbeResult &probe) {
  if (!probe.has_intel_jpeg_encoder() || !probe.has_intel_jpeg_decoder()) {
    std::printf("[HW] 跳过：没有支持 JPEG 的 Intel 硬件单元\n");
    return true;
  }
  const uint32_t width = 320;
  const uint32_t height = 240;
  std::vector<uint8_t> rgb = oic::selftest::make_test_image(width, height);

  VplJpegEncoder encoder;
  if (!encoder.init(width, height, kDefaultQuality)) {
    std::printf("[HW] 编码器初始化失败：%s\n", encoder.last_error.c_str());
    return false;
  }
  std::vector<uint8_t> jpeg;
  if (!encoder.encode_rgb24(rgb.data(), rgb.size(), jpeg)) {
    std::printf("[HW] 编码失败：%s\n", encoder.last_error.c_str());
    return false;
  }
  const std::string encode_impl = encoder.impl_name;
  encoder.close();

  VplJpegDecoder decoder;
  if (!decoder.init()) {
    std::printf("[HW] 解码器初始化失败：%s\n", decoder.last_error.c_str());
    return false;
  }
  std::vector<uint8_t> bgr;
  uint32_t decoded_w = 0;
  uint32_t decoded_h = 0;
  if (!decoder.decode_bgr24(jpeg.data(), jpeg.size(), bgr, decoded_w,
                            decoded_h)) {
    std::printf("[HW] 解码失败：%s\n", decoder.last_error.c_str());
    return false;
  }
  const std::string decode_impl = decoder.impl_name;
  decoder.close();
  if (decoded_w != width || decoded_h != height) {
    std::printf("[HW] 失败：解码尺寸 %ux%u != %ux%u\n", decoded_w, decoded_h,
                width, height);
    return false;
  }
  std::vector<uint8_t> decoded_rgb(bgr.size());
  for (size_t i = 0; i + 2 < bgr.size(); i += 3) {
    decoded_rgb[i + 0] = bgr[i + 2];
    decoded_rgb[i + 1] = bgr[i + 1];
    decoded_rgb[i + 2] = bgr[i + 0];
  }
  const double psnr =
      oic::selftest::psnr_rgb(rgb.data(), decoded_rgb.data(), rgb.size());
  std::printf("[HW] JPEG 往返：%ux%u q=%u，编码=%s，解码=%s，PSNR=%.2f dB\n",
              width, height, kDefaultQuality, encode_impl.c_str(),
              decode_impl.c_str(), psnr);
  if (psnr < 24.0) {
    std::printf("[HW] 失败：PSNR 过低\n");
    return false;
  }
  return true;
}

// 硬件 JPEG strips -> TIFF，再用 libtiff 解码校验。
bool selftest_hardware_tiff(const VplProbeResult &probe) {
  if (!probe.has_intel_jpeg_encoder()) {
    std::printf("[HW] TIFF 跳过：没有 Intel JPEG 编码单元\n");
    return true;
  }
  if (!oic::selftest::have_tiff_decoder()) {
    std::printf("[HW] TIFF 跳过：缺少 libtiff（无法校验）\n");
    return true;
  }
  // 故意用奇数宽高 + 32 行 strip（末条只有 1 行）：这是最容易踩硬件下限的
  // 组合——NV12 表面要求偶数宽高，Intel JPEG 编码器还要求表面至少 16 行。
  // 编码器把表面补齐、CropH 保留真实高度，SOF 才能与 TIFF 的 strip 尺寸对上。
  const uint32_t width = 257;
  const uint32_t height = 193;
  const uint32_t rows = 32;
  const std::vector<uint8_t> rgb =
      oic::selftest::make_test_image(width, height);

  HardwareStripEncoder hardware;
  hardware.quality = kDefaultQuality;
  oic::tiff::JpegStripEncoder encoder;
  encoder.quality = kDefaultQuality;
  encoder.encode = [&hardware](const uint8_t *strip_rgb, uint32_t strip_width,
                               uint32_t strip_rows, std::vector<uint8_t> &jpeg,
                               std::string &error) {
    const bool ok = hardware(strip_rgb, strip_width, strip_rows, jpeg);
    if (!ok) {
      error = hardware.error;
    }
    return ok;
  };

  const std::string path = "icp_selftest_hw_jpeg.tiff";
  oic::tiff::JpegStripTiffReport report;
  std::string error;
  if (!oic::tiff::write_tiff_jpeg_strips(path.c_str(), rgb.data(), width,
                                         height, rows, encoder, report,
                                         error)) {
    std::printf("[HW] TIFF 写出失败：%s\n", error.c_str());
    return false;
  }

  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    std::printf("[HW] TIFF 读回失败\n");
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::vector<uint8_t> tiff(static_cast<size_t>(size));
  const bool read_ok =
      std::fread(tiff.data(), 1, tiff.size(), file) == tiff.size();
  std::fclose(file);
  std::remove(path.c_str());
  if (!read_ok) {
    std::printf("[HW] TIFF 读回失败\n");
    return false;
  }

  uint32_t decoded_w = 0;
  uint32_t decoded_h = 0;
  std::vector<uint8_t> decoded;
  if (!oic::selftest::decode_tiff_reference(tiff, decoded_w, decoded_h, decoded,
                                            error)) {
    std::printf("[HW] libtiff 解码 TIFF 失败：%s\n", error.c_str());
    return false;
  }
  if (decoded_w != width || decoded_h != height) {
    std::printf("[HW] TIFF 尺寸不符：%ux%u\n", decoded_w, decoded_h);
    return false;
  }
  const double psnr =
      oic::selftest::psnr_rgb(rgb.data(), decoded.data(), rgb.size());
  std::printf("[HW] TIFF（%u 条硬件 JPEG strips）libtiff 解码 PSNR=%.2f dB\n",
              report.strips, psnr);
  return psnr >= 24.0;
}

int run_selftest(bool require_hardware) {
  const VplProbeResult probe = oic::jpeg::onevpl::vpl_probe();
  std::printf("%s\n", probe.describe().c_str());

  bool skipped = false;
  bool ok = selftest_tiff_container(skipped);

  const bool hardware =
      probe.has_intel_jpeg_encoder() && probe.has_intel_jpeg_decoder();
  if (hardware) {
    ok = selftest_hardware_jpeg(probe) && ok;
    ok = selftest_hardware_tiff(probe) && ok;
  } else {
    std::printf(
        "[HW] 跳过：本机没有可用的 Intel 媒体引擎（需要 vpl-gpu-rt）\n");
    if (require_hardware) {
      std::printf("结果：FAIL（--require-hardware 要求硬件路径必须可跑）\n");
      return 1;
    }
  }

  if (!ok) {
    std::printf("结果：FAIL\n");
    return 1;
  }
  std::printf("结果：PASS%s\n", skipped ? "（部分自测因缺依赖跳过）" : "");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc <= 1) {
    return run_selftest(false);
  }
  const std::string command = argv[1];
  if (command == "units") {
    return command_units();
  }
  if (command == "selftest") {
    bool require_hardware = false;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--require-hardware") == 0) {
        require_hardware = true;
      }
    }
    return run_selftest(require_hardware);
  }
  if (command == "encode") {
    if (argc < 4) {
      print_usage();
      return 2;
    }
    const int quality =
        argc >= 5 ? std::atoi(argv[4]) : static_cast<int>(kDefaultQuality);
    return oic_onevpl_encode(argv[2], argv[3], quality);
  }
  if (command == "decode") {
    if (argc < 4) {
      print_usage();
      return 2;
    }
    return oic_onevpl_decode(argv[2], argv[3]);
  }
  if (command == "tiff") {
    return command_tiff(argc, argv);
  }
  print_usage();
  return 2;
}
