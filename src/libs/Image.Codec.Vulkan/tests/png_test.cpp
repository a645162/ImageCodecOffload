// File      : png_test.cpp
// Brief     : PNG 编解码自测（GPU 滤波 vs CPU 参考、1..4 通道往返、band 边界）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 退出码：0 = PASS，2 = SKIP（没有可用 Vulkan 设备），1 = FAIL。
//
// 为什么要和 CPU 参考逐字节对拍：PNG 的 filtered 流只要有一个字节不同，
// 编出来的文件在人眼里完全一样（zlib 照样压缩、libpng 照样能读），
// 但「GPU 滤波到底有没有算对」就再也无法验证了。
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"
#include "png_codec.h"
#include "png_container.h"
#include "png_filter_vk.h"
#include "vk_codec_device.h"

namespace {

using image_codec::vulkan::ImageBuffer;

int g_failures = 0;

void check(bool ok, const std::string &what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

ImageBuffer makeImage(int width, int height, int channels, unsigned seed) {
  ImageBuffer image;
  if (!image.allocate(width, height, channels)) return image;
  unsigned state = seed * 2654435761u + 1u;
  for (size_t i = 0; i < image.data.size(); ++i) {
    state = state * 1103515245u + 12345u;
    image.data[i] = static_cast<uint8_t>((state >> 16) & 0xFF);
  }
  return image;
}

// GPU 滤波结果必须与 CPU 参考实现逐字节一致（自适应与固定滤波都比）。
void checkFilterParity() {
  using namespace image_codec::vulkan::png;

  for (const int channels : {1, 2, 3, 4}) {
    for (const int band_rows : {1, 7, 64, 256}) {
      const ImageBuffer img = makeImage(37, 23, channels, 7u + channels);

      std::vector<uint8_t> gpu;
      std::string err;
      FilterOptions options;
      options.band_rows = band_rows;
      if (!filterRowsOnGpu(img, options, &gpu, &err)) {
        check(false, "gpu filter (ch=" + std::to_string(channels) +
                         ", band=" + std::to_string(band_rows) + "): " + err);
        continue;
      }

      const std::vector<uint8_t> cpu =
          buildFilteredStream(img.data.data(), img.width, img.height,
                              img.channels, chooseAdaptiveFilters(
                                                 img.data.data(), img.width,
                                                 img.height, img.channels));

      check(gpu == cpu, "gpu adaptive filter == cpu (ch=" +
                            std::to_string(channels) + ", band=" +
                            std::to_string(band_rows) + ")");
    }
  }

  // 固定滤波类型：GPU 与 CPU 必须同样逐字节一致。
  const ImageBuffer img = makeImage(41, 19, 3, 99u);
  for (int filter = 0; filter <= 4; ++filter) {
    std::vector<uint8_t> gpu;
    std::string err;
    FilterOptions options;
    options.filter = filter;
    options.band_rows = 5;
    if (!filterRowsOnGpu(img, options, &gpu, &err)) {
      check(false, "gpu fixed filter: " + err);
      continue;
    }
    std::vector<uint8_t> filters(static_cast<size_t>(img.height),
                                 static_cast<uint8_t>(filter));
    const std::vector<uint8_t> cpu =
        buildFilteredStream(img.data.data(), img.width, img.height,
                            img.channels, filters);
    check(gpu == cpu, "gpu fixed filter == cpu (filter=" +
                          std::to_string(filter) + ")");
  }
}

// 编码 -> 解码必须逐位一致（PNG 是无损格式，没有「差不多」）。
void checkRoundtrip() {
  using namespace image_codec::vulkan::png;

  for (const int channels : {1, 2, 3, 4}) {
    const ImageBuffer img = makeImage(53, 37, channels, 3u + channels);

    for (const int band_rows : {4, 256}) {
      PngEncodeOptions options;
      options.band_rows = band_rows;

      std::vector<uint8_t> encoded;
      bool gpu_used = false;
      std::string err;
      if (!encodePng(img, options, &encoded, &gpu_used, &err)) {
        check(false, "png encode (ch=" + std::to_string(channels) + "): " + err);
        continue;
      }
      check(gpu_used, "png encode used GPU (ch=" + std::to_string(channels) + ")");
      check(hasPngSignature(encoded.data(), encoded.size()), "png signature");

      ImageBuffer decoded;
      if (!decodePng(encoded.data(), encoded.size(), &decoded, &err)) {
        check(false, "png decode (ch=" + std::to_string(channels) + "): " + err);
        continue;
      }
      check(decoded.width == img.width && decoded.height == img.height,
            "png geometry");
      check(decoded.channels == img.channels, "png channel fidelity");
      check(decoded.data == img.data, "png lossless roundtrip (ch=" +
                                          std::to_string(channels) + ")");
    }
  }
}

// zlib 压缩级别 0（不压缩）与 9 都必须产出可解码的流。
void checkCompressionLevels() {
  using namespace image_codec::vulkan::png;
  const ImageBuffer img = makeImage(31, 17, 3, 11u);

  for (const int level : {0, 1, 6, 9}) {
    PngEncodeOptions options;
    options.compression_level = level;
    std::vector<uint8_t> encoded;
    bool gpu_used = false;
    std::string err;
    if (!encodePng(img, options, &encoded, &gpu_used, &err)) {
      check(false, "png encode level " + std::to_string(level) + ": " + err);
      continue;
    }
    ImageBuffer decoded;
    check(decodePng(encoded.data(), encoded.size(), &decoded, &err),
          "png decode level " + std::to_string(level) + ": " + err);
    check(decoded.data == img.data,
          "png roundtrip level " + std::to_string(level));
  }
}

// GPU 滤波失败时必须明确报错，且 allow_cpu_fallback 决定是回退还是失败。
void checkBudgetAndFallback() {
  using namespace image_codec::vulkan::png;
  const ImageBuffer img = makeImage(64, 64, 3, 5u);

  FilterOptions tiny;
  tiny.memory_limit = 1024;  // 远小于一个 band
  std::vector<uint8_t> gpu;
  std::string err;
  check(!filterRowsOnGpu(img, tiny, &gpu, &err), "tiny budget rejects band");
  check(!err.empty(), "tiny budget explains why");

  PngEncodeOptions fallback;
  fallback.memory_limit = 1024;
  std::vector<uint8_t> encoded;
  bool gpu_used = true;
  check(encodePng(img, fallback, &encoded, &gpu_used, &err),
        "cpu fallback still produces a png: " + err);
  check(!gpu_used, "fallback reports gpu_used=false");

  PngEncodeOptions strict;
  strict.memory_limit = 1024;
  strict.allow_cpu_fallback = false;
  encoded.clear();
  check(!encodePng(img, strict, &encoded, &gpu_used, &err),
        "no fallback means encode fails");

  ImageBuffer decoded;
  std::string decode_err;
  if (decodePng(encoded.data(), encoded.size(), &decoded, &decode_err)) {
    check(false, "empty/garbage png must not decode");
  }
}

}  // namespace

int main() {
  image_codec::vulkan::Device probe;
  std::string device_error;
  if (!probe.init(0, &device_error)) {
    std::printf("SKIP: %s\n", device_error.c_str());
    return 2;
  }
  std::printf("device: %s\n", probe.caps().name.c_str());

  checkFilterParity();
  checkRoundtrip();
  checkCompressionLevels();
  checkBudgetAndFallback();

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}
