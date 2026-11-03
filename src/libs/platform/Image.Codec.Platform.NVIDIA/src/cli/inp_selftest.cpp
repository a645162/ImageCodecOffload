/*
 * File: inp_selftest.cpp
 * Brief: Self-test for the NVIDIA platform codecs.
 * Author: Haomin Kong
 * Email: konghm@beim.com.cn / a645162@163.com
 * Created: 2026-11-03
 */

// inp_selftest.cpp - 本机自测：设备枚举 + nvJPEG / nvJPEG2000 / nvTIFF 往返。
//
// 这不是 ctest 套件（本工程按需在开发机上手跑），所以判据写成「能编译进来、
// 能跑通、能对得上像素」，缺某个厂商库时对应项明确 SKIP 而不是整个程序失败。

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "image_buffer.h"
#include "nvidia_runtime.h"

#if defined(OIC_NVIDIA_WITH_JPEG)
#include "nvjpeg_codec.h"
#endif
#if defined(OIC_NVIDIA_WITH_JPEG2K)
#include "nvjpeg2k_codec.h"
#endif
#if defined(OIC_NVIDIA_WITH_TIFF)
#include "nvtiff_codec.h"
#endif

namespace {

int failures = 0;
int skipped = 0;

void check(bool ok, const std::string &what) {
  std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
  if (!ok) {
    ++failures;
  }
}

void skip(const std::string &what) {
  std::printf("  [SKIP] %s\n", what.c_str());
  ++skipped;
}

// 有梯度的测试图（纯色会让有损阈值失去意义，也测不到行序错误）。
image_codec::nvidia::ImageBuffer make_test_image(int width, int height,
                                                 int channels) {
  image_codec::nvidia::ImageBuffer img;
  img.allocate(width, height, channels);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t offset =
          (static_cast<size_t>(y) * static_cast<size_t>(width) +
           static_cast<size_t>(x)) *
          static_cast<size_t>(channels);
      const uint8_t base =
          static_cast<uint8_t>((x * 255) / (width > 1 ? width - 1 : 1));
      const uint8_t other =
          static_cast<uint8_t>((y * 255) / (height > 1 ? height - 1 : 1));
      for (int c = 0; c < channels; ++c) {
        img.data[offset + static_cast<size_t>(c)] =
            static_cast<uint8_t>((base + other * (c + 1)) / (c + 2));
      }
    }
  }
  return img;
}

// PSNR（dB）；完全相同时返回 +inf。
double psnr(const image_codec::nvidia::ImageBuffer &a,
            const image_codec::nvidia::ImageBuffer &b) {
  if (a.width != b.width || a.height != b.height || a.channels != b.channels) {
    return 0.0;
  }
  double sum = 0.0;
  for (size_t i = 0; i < a.data.size(); ++i) {
    const double d =
        static_cast<double>(a.data[i]) - static_cast<double>(b.data[i]);
    sum += d * d;
  }
  if (sum == 0.0) {
    return std::numeric_limits<double>::infinity();
  }
  const double mse = sum / static_cast<double>(a.data.size());
  return 10.0 * std::log10((255.0 * 255.0) / mse);
}

template <typename EncodeFunc, typename DecodeFunc>
void roundtrip_lossy(const char *name, EncodeFunc encode, DecodeFunc decode,
                     const image_codec::nvidia::ImageBuffer &source,
                     double min_psnr) {
  std::vector<uint8_t> bytes;
  std::string err;
  if (!encode(source, &bytes, &err)) {
    check(false, std::string(name) + " encode: " + err);
    return;
  }
  check(!bytes.empty(), std::string(name) + " produced a bitstream");
  image_codec::nvidia::ImageBuffer back;
  if (!decode(bytes, &back, &err)) {
    check(false, std::string(name) + " decode: " + err);
    return;
  }
  check(back.width == source.width && back.height == source.height &&
            back.channels == source.channels,
        std::string(name) + " roundtrip keeps geometry / channels");
  const double value = psnr(source, back);
  std::printf("         PSNR = %.2f dB (threshold %.1f)\n", value, min_psnr);
  check(value >= min_psnr, std::string(name) + " PSNR above threshold");
}

} // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  std::printf("Image.Codec.Platform.NVIDIA self-test\n");
  const auto devices = image_codec::nvidia::enumerate_devices();
  if (devices.empty()) {
    std::printf("  no CUDA device; nothing to test\n");
    return 1;
  }
  for (const auto &device : devices) {
    std::printf("  device %d: %s (sm_%d%d, %.1f GiB, driver %s, runtime %s)\n",
                device.index, device.name.c_str(), device.major, device.minor,
                static_cast<double>(device.total_memory) /
                    (1024.0 * 1024.0 * 1024.0),
                device.driver_version.c_str(), device.runtime_version.c_str());
  }
  const int device_index = devices.front().index;
  const auto rgb = make_test_image(321, 257, 3);
  const auto gray = make_test_image(257, 129, 1);

#if defined(OIC_NVIDIA_WITH_JPEG)
  std::printf("nvJPEG:\n");
  check(image_codec::nvidia::jpeg::available(), "available");
  std::printf("         version = %s\n",
              image_codec::nvidia::jpeg::version().c_str());
  if (!image_codec::nvidia::jpeg::available()) {
    skip("nvJPEG is not available in this build");
  } else {
    for (const auto *img : {&rgb, &gray}) {
      const std::string label = img->channels == 1 ? "gray" : "rgb";
      roundtrip_lossy(
          ("jpeg " + label).c_str(),
          [&](const image_codec::nvidia::ImageBuffer &src,
              std::vector<uint8_t> *out, std::string *e) {
            return image_codec::nvidia::jpeg::encode(device_index, src, 95, 0,
                                                     out, e);
          },
          [&](const std::vector<uint8_t> &in,
              image_codec::nvidia::ImageBuffer *out, std::string *e) {
            return image_codec::nvidia::jpeg::decode(device_index, in.data(),
                                                     in.size(), out, e);
          },
          *img, 32.0);
    }
  }
#endif

#if defined(OIC_NVIDIA_WITH_JPEG2K)
  std::printf("nvJPEG2000:\n");
  check(image_codec::nvidia::jpeg2k::available(), "available");
  std::printf("         version = %s\n",
              image_codec::nvidia::jpeg2k::version().c_str());
  if (!image_codec::nvidia::jpeg2k::available()) {
    skip("nvJPEG2000 is not available in this build");
  } else {
    for (const auto *img : {&rgb, &gray}) {
      const std::string label = img->channels == 1 ? "gray" : "rgb";
      std::vector<uint8_t> bytes;
      std::string err;
      if (!image_codec::nvidia::jpeg2k::encode(device_index, *img, 0, &bytes,
                                               &err)) {
        check(false, "jpeg2k " + label + " lossless encode: " + err);
      } else {
        image_codec::nvidia::ImageBuffer back;
        if (!image_codec::nvidia::jpeg2k::decode(device_index, bytes.data(),
                                                 bytes.size(), &back, &err)) {
          check(false, "jpeg2k " + label + " lossless decode: " + err);
        } else {
          check(back.width == img->width && back.height == img->height &&
                    back.channels == img->channels,
                "jpeg2k " + label + " lossless keeps geometry / channels");
          check(back.data == img->data,
                "jpeg2k " + label + " lossless is exact");
        }
      }
      roundtrip_lossy(
          ("jpeg2k " + label + " lossy").c_str(),
          [&](const image_codec::nvidia::ImageBuffer &src,
              std::vector<uint8_t> *out, std::string *e) {
            return image_codec::nvidia::jpeg2k::encode(device_index, src, 90,
                                                       out, e);
          },
          [&](const std::vector<uint8_t> &in,
              image_codec::nvidia::ImageBuffer *out, std::string *e) {
            return image_codec::nvidia::jpeg2k::decode(device_index, in.data(),
                                                       in.size(), out, e);
          },
          *img, 30.0);
    }
  }
#endif

#if defined(OIC_NVIDIA_WITH_TIFF)
  std::printf("nvTIFF:\n");
  check(image_codec::nvidia::tiff::available(), "available");
  std::printf("         version = %s\n",
              image_codec::nvidia::tiff::version().c_str());
  if (!image_codec::nvidia::tiff::available()) {
    skip("nvTIFF is not available in this build");
  } else {
    for (const auto *img : {&rgb, &gray}) {
      for (const int compression : {1, 5, 8}) {
        const std::string label =
            std::string(img->channels == 1 ? "gray" : "rgb") +
            " compression=" + std::to_string(compression);
        image_codec::nvidia::tiff::EncodeOptions options;
        options.compression = compression;
        std::vector<uint8_t> bytes;
        std::string err;
        if (!image_codec::nvidia::tiff::encode(device_index, *img, options,
                                               &bytes, &err)) {
          check(false, "tiff " + label + " encode: " + err);
          continue;
        }
        image_codec::nvidia::ImageBuffer back;
        if (!image_codec::nvidia::tiff::decode(device_index, bytes.data(),
                                               bytes.size(), &back, &err)) {
          check(false, "tiff " + label + " decode: " + err);
          continue;
        }
        // NONE / LZW / Deflate 都是无损：必须逐字节相等。
        check(back.width == img->width && back.height == img->height &&
                  back.channels == img->channels,
              "tiff " + label + " keeps geometry / channels");
        check(back.data == img->data, "tiff " + label + " is lossless");
      }
    }
  }
#endif

  std::printf("\n%s (%d failure(s), %d skipped)\n",
              failures == 0 ? "PASS" : "FAIL", failures, skipped);
  return failures == 0 ? 0 : 1;
}
