// File      : tiff_test.cpp
// Brief     : TIFF 编解码自测（GPU 预测器 vs CPU 参考、1..4 通道往返、strip/tile）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 退出码：0 = PASS，2 = SKIP（没有可用 Vulkan 设备），1 = FAIL。
//
// 为什么预测器必须逐字节对拍：Predictor=2 只是一个可逆变换，写错了压缩率会变、
// 但 TIFF 一样能打开——除非解码端也用同一套错误，否则「看起来都能用」。
// 因此 GPU 结果要与 CPU 参考严格相等，并且往返之后必须逐位还原。
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "image_buffer.h"
#include "lzw_codec.h"
#include "tiff_codec.h"
#include "tiff_predict_vk.h"
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
  std::mt19937 rng(seed);
  for (auto &v : image.data) v = static_cast<uint8_t>(rng() & 0xFF);
  return image;
}

// ---- 0. LZW 码流本身（最容易被当成「已经是老代码」而漏测的一层）----
//
// 这一段是回归测试：码宽在 512/1024/2048 处增长时，编码器在**最后一个数据码**
// 之后也必须按解码器的建表进度对齐宽度，否则 EOI 会用错位宽，表现为
// 「缺 EOI」或解出多余字节。随机长度 + 大块数据能把三个边界都走到。
void checkLzwCodec() {
  using namespace image_codec::vulkan::lzw;

  std::mt19937 rng(2026);
  int failures = 0;
  for (int round = 0; round < 2000; ++round) {
    // 200..1500 覆盖 9→10 与 10→11 两个边界附近。
    const size_t n = 200 + (rng() % 1300);
    std::vector<uint8_t> input(n);
    const int mode = round % 3;
    for (auto &v : input) {
      if (mode == 0) {
        v = static_cast<uint8_t>(rng() & 0xFF);
      } else if (mode == 1) {
        v = (rng() % 3u == 0u) ? static_cast<uint8_t>(rng() & 0xFF) : 0u;
      } else {
        v = static_cast<uint8_t>(rng() & 0x0F);
      }
    }

    std::vector<uint8_t> compressed;
    std::vector<uint8_t> restored;
    if (encode(input.data(), input.size(), compressed) != 0 ||
        decode(compressed.data(), compressed.size(), restored) != 0 ||
        restored != input) {
      ++failures;
    }
  }
  check(failures == 0, "lzw codec roundtrip across code-width boundaries (" +
                           std::to_string(failures) + " failures)");

  // 大于字典容量（4096 项）的输入必然会发 ClearCode，顺带覆盖重置路径。
  std::vector<uint8_t> big(200u * 1024u);
  for (auto &v : big) v = static_cast<uint8_t>(rng() & 0xFF);

  std::vector<uint8_t> compressed;
  std::vector<uint8_t> restored;
  check(encode(big.data(), big.size(), compressed) == 0,
        "lzw encode big block");
  check(decode(compressed.data(), compressed.size(), restored) == 0,
        "lzw decode big block");
  check(restored == big, "lzw big block roundtrip");
}

// ---- 1. CPU 参考本身 ----
void checkCpuPredictor() {
  using namespace image_codec::vulkan::tiff;

  for (const int channels : {1, 2, 3, 4}) {
    const ImageBuffer img = makeImage(37, 9, channels, 11u + channels);
    std::vector<uint8_t> predicted(img.data.size(), 0);
    predictRowsCpu(img.data.data(), img.stride(), channels, img.height,
                   predicted.data());

    // 原地版本必须与「另开输出」的参考逐字节相同。
    std::vector<uint8_t> inplace = img.data;
    predictRowsCpuInPlace(inplace.data(), img.stride(), channels, img.height);
    check(inplace == predicted, "cpu in-place predictor == reference (ch=" +
                                    std::to_string(channels) + ")");

    unpredictRowsCpu(inplace.data(), img.stride(), channels, img.height);
    check(inplace == img.data,
          "cpu predictor roundtrip (ch=" + std::to_string(channels) + ")");
  }
}

// ---- 2. GPU 预测器 vs CPU 参考 ----
void checkGpuPredictor() {
  using namespace image_codec::vulkan::tiff;

  PredictorGpu predictor;
  std::string err;
  if (!predictor.init(0, -1, &err)) {
    check(false, "predictor init: " + err);
    return;
  }

  for (const int channels : {1, 2, 3, 4}) {
    // 37 * channels 不是 4 的倍数：尾部跨字的那几个字节最容易写错。
    const ImageBuffer img = makeImage(37, 23, channels, 7u + channels);

    for (const int band_rows : {1, 5, 23}) {
      std::vector<uint8_t> reference(img.data.size(), 0);
      predictRowsCpu(img.data.data(), img.stride(), channels, img.height,
                     reference.data());

      std::vector<uint8_t> gpu = img.data;
      if (!predictor.run(gpu.data(), img.stride(), channels, img.height,
                         band_rows, &err)) {
        check(false, "gpu predictor: " + err);
        continue;
      }
      check(gpu == reference,
            "gpu predictor == cpu (ch=" + std::to_string(channels) +
                ", band=" + std::to_string(band_rows) + ")");
    }
  }
}

// ---- 3. 编码/解码往返 ----
void checkRoundtrip() {
  using namespace image_codec::vulkan::tiff;

  for (const int channels : {1, 2, 3, 4}) {
    const ImageBuffer img = makeImage(53, 37, channels, 3u + channels);

    // strip + LZW + 预测器（默认路径）
    {
      TiffEncodeOptions options;
      std::vector<uint8_t> encoded;
      bool gpu_used = false;
      std::string err;
      if (!encodeTiff(img, options, &encoded, &gpu_used, &err)) {
        check(false, "tiff encode (strip, ch=" + std::to_string(channels) +
                         "): " + err);
        continue;
      }
      check(gpu_used, "tiff strip encode used GPU predictor (ch=" +
                          std::to_string(channels) + ")");

      ImageBuffer decoded;
      if (!decodeTiff(encoded.data(), encoded.size(), &decoded, &err)) {
        check(false, "tiff decode (strip, ch=" + std::to_string(channels) +
                         "): " + err);
        continue;
      }
      check(decoded.channels == img.channels, "tiff channel fidelity");
      check(decoded.width == img.width && decoded.height == img.height,
            "tiff geometry");
      check(decoded.data == img.data, "tiff strip lossless roundtrip (ch=" +
                                          std::to_string(channels) + ")");
    }

    // tiled + LZW + 预测器：tile 边界与 padding 是最容易错的地方。
    for (const int tile_size : {16, 32}) {
      TiffEncodeOptions options;
      options.tile_size = tile_size;
      options.band_rows = 64;

      std::vector<uint8_t> encoded;
      bool gpu_used = false;
      std::string err;
      if (!encodeTiff(img, options, &encoded, &gpu_used, &err)) {
        check(false,
              "tiff encode (tile=" + std::to_string(tile_size) + "): " + err);
        continue;
      }
      ImageBuffer decoded;
      if (!decodeTiff(encoded.data(), encoded.size(), &decoded, &err)) {
        check(false,
              "tiff decode (tile=" + std::to_string(tile_size) + "): " + err);
        continue;
      }
      check(decoded.data == img.data,
            "tiff tiled lossless roundtrip (tile=" + std::to_string(tile_size) +
                ", ch=" + std::to_string(channels) + ")");
    }

    // 多 strip：覆盖 strip 偏移/长度数组（>1 项）与「一批 strip 就是一段
    // 连续行」的前提。单 strip 走不到这些路径。
    {
      TiffEncodeOptions options;
      options.rows_per_strip = 8;
      options.band_rows = 16;

      std::vector<uint8_t> encoded;
      bool gpu_used = false;
      std::string err;
      if (!encodeTiff(img, options, &encoded, &gpu_used, &err)) {
        check(false, "tiff encode (multi-strip): " + err);
      } else {
        check(gpu_used, "multi-strip encode used GPU predictor");
        ImageBuffer decoded;
        if (!decodeTiff(encoded.data(), encoded.size(), &decoded, &err)) {
          check(false, "tiff decode (multi-strip): " + err);
        } else {
          check(decoded.data == img.data,
                "multi-strip lossless roundtrip (ch=" +
                    std::to_string(channels) + ")");
        }
      }
    }

    // 未压缩：不带预测器，GPU 不参与。
    {
      TiffEncodeOptions options;
      options.compression = 1;
      std::vector<uint8_t> encoded;
      bool gpu_used = true;
      std::string err;
      if (!encodeTiff(img, options, &encoded, &gpu_used, &err)) {
        check(false, "tiff encode (none): " + err);
        continue;
      }
      check(!gpu_used, "uncompressed tiff does not use GPU");
      ImageBuffer decoded;
      check(decodeTiff(encoded.data(), encoded.size(), &decoded, &err),
            "tiff decode (none): " + err);
      check(decoded.data == img.data, "uncompressed tiff roundtrip");
    }
  }
}

// 预测器关闭时 GPU 同样不该被使用，产物仍必须可解码。
void checkPredictorDisabled() {
  using namespace image_codec::vulkan::tiff;
  const ImageBuffer img = makeImage(29, 13, 3, 41u);

  TiffEncodeOptions options;
  options.predictor = false;
  std::vector<uint8_t> encoded;
  bool gpu_used = true;
  std::string err;
  if (!encodeTiff(img, options, &encoded, &gpu_used, &err)) {
    check(false, "tiff encode without predictor: " + err);
    return;
  }
  check(!gpu_used, "predictor disabled means gpu_used=false");

  ImageBuffer decoded;
  check(decodeTiff(encoded.data(), encoded.size(), &decoded, &err),
        "tiff decode without predictor: " + err);
  check(decoded.data == img.data, "roundtrip without predictor");
}

// ---- 4. 显存预算与回退 ----
void checkBudgetAndFallback() {
  using namespace image_codec::vulkan::tiff;
  const ImageBuffer img = makeImage(64, 64, 3, 5u);

  PredictorGpu predictor;
  std::string err;
  if (!predictor.init(1024, -1, &err)) {
    check(false, "tiny-budget predictor init: " + err);
    return;
  }
  std::vector<uint8_t> data = img.data;
  check(!predictor.run(data.data(), img.stride(), img.channels, img.height, 64,
                       &err),
        "tiny budget rejects predictor band");
  check(!err.empty(), "tiny budget explains why");

  // 允许回退：产物必须仍然正确，只是 gpu_used 为 false。
  TiffEncodeOptions fallback;
  fallback.memory_limit = 1024;
  std::vector<uint8_t> encoded;
  bool gpu_used = true;
  check(encodeTiff(img, fallback, &encoded, &gpu_used, &err),
        "cpu predictor fallback still encodes: " + err);
  check(!gpu_used, "fallback reports gpu_used=false");
  ImageBuffer decoded;
  check(decodeTiff(encoded.data(), encoded.size(), &decoded, &err),
        "fallback output decodes: " + err);
  check(decoded.data == img.data, "fallback output is lossless");

  // 不允许回退：必须明确失败，而不是悄悄用 CPU。
  TiffEncodeOptions strict;
  strict.memory_limit = 1024;
  strict.allow_cpu_fallback = false;
  encoded.clear();
  check(!encodeTiff(img, strict, &encoded, &gpu_used, &err),
        "no fallback means encode fails");
}

// ---- 5. 损坏输入必须报错而不是崩 ----
void checkInvalidInput() {
  using namespace image_codec::vulkan::tiff;
  const ImageBuffer img = makeImage(17, 11, 3, 77u);

  TiffEncodeOptions options;
  std::vector<uint8_t> encoded;
  bool gpu_used = false;
  std::string err;
  if (!encodeTiff(img, options, &encoded, &gpu_used, &err)) {
    check(false, "encode for invalid-input test: " + err);
    return;
  }

  ImageBuffer decoded;
  check(!decodeTiff(nullptr, 0, &decoded, &err), "null input rejected");
  check(!decodeTiff(encoded.data(), 4, &decoded, &err),
        "truncated header rejected");
  check(!decodeTiff(encoded.data(), encoded.size() / 2, &decoded, &err),
        "truncated payload rejected");

  // 通道数非法也必须拒绝（不是悄悄改成 3 通道）。
  ImageBuffer bad;
  bad.width = 4;
  bad.height = 4;
  bad.channels = 5;
  bad.data.assign(80, 0);
  std::vector<uint8_t> out;
  check(!encodeTiff(bad, options, &out, &gpu_used, &err),
        "5-channel image rejected");
}

} // namespace

int main() {
  // 先跑不需要设备的码流测试：没有 Vulkan 的机器也应该验证 LZW/容器，
  // 否则「环境 SKIP」会把真正的编码缺陷一起藏起来。
  checkLzwCodec();
  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }

  image_codec::vulkan::Device probe;
  std::string device_error;
  if (!probe.init(-1, &device_error)) {
    std::printf("SKIP: %s\n", device_error.c_str());
    return 2;
  }
  std::printf("device: %s\n", probe.caps().name.c_str());

  checkCpuPredictor();
  checkGpuPredictor();
  checkRoundtrip();
  checkPredictorDisabled();
  checkBudgetAndFallback();
  checkInvalidInput();

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}
