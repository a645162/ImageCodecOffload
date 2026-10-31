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
#include <random>
#include <string>
#include <vector>

#include "bmp_io.h"
#include "image_codec_types.h"
#include "metrics.h"
#include "router.h"

using image_codec::offload::BackendKind;
using image_codec::offload::EncodeParams;
using image_codec::offload::Format;
using image_codec::offload::Image;
using image_codec::offload::Router;
using image_codec::offload::SsimResult;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
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

Image perturb(const Image& src, unsigned seed) {
  Image out = src;
  std::mt19937 rng(seed);
  for (auto& v : out.pixels) {
    const int d = static_cast<int>(rng() % 5) - 2;
    const int nv = static_cast<int>(v) + d;
    v = static_cast<uint8_t>(nv < 0 ? 0 : (nv > 255 ? 255 : nv));
  }
  return out;
}

// ---- 1. BMP 中转 ----

void testBmpRoundtrip() {
  for (int channels : {1, 3}) {
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
}

// ---- 2. CPU 基线往返 ----

void testCpuRoundtrip() {
  Router router;
  const Image src = makeImage(257, 129, 3);  // 故意不是 16 的倍数

  // TIFF 无压缩：无损，必须逐位一致。
  {
    EncodeParams params;
    params.tiff_compression = 1;
    auto encoded = router.encodeWith(BackendKind::Cpu, Format::Tiff, src, params);
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
    auto encoded = router.encodeWith(BackendKind::Cpu, Format::Tiff, src, params);
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
    auto encoded = router.encodeWith(BackendKind::Cpu, Format::Tiff, src, params);
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
    auto encoded = router.encodeWith(BackendKind::Cpu, Format::Png, src, params);
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
  if (mse) check(std::fabs(*mse - kGoldenMse) < 1e-9, "golden mse matches CUDA");

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
  const Image a = makeImage(320, 240, 3);
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
      continue;  // 未编译进来 / 无设备：不是失败
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

    std::printf("%s: mse=%.9f psnr=%.6f ssim=%.9f ms-ssim=%.9f\n",
                image_codec::offload::backendName(kind), *mse, psnr ? *psnr : 0.0,
                ssim ? ssim->ssim : 0.0, ssim ? ssim->ms_ssim : 0.0);
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

  // 强制指定 OpenCL：它的 C ABI 只有 24-bit RGB，所以必须**失败**，
  // 而不是返回一张把灰度复制了三遍的图。
  EncodeParams params;
  auto jpeg = router.encodeWith(BackendKind::Cpu, Format::Jpeg, gray, params);
  if (jpeg) {
    const auto forced =
        router.decodeWith(BackendKind::OpenCl, Format::Jpeg, jpeg->data(),
                          jpeg->size());
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

void testFallbackLeavesReason() {
  Router router;
  // 只留 OpenCL + CPU：灰度图在 OpenCL 上不受支持，必须回退到 CPU。
  router.setOrder({BackendKind::OpenCl});

  const Image gray = makeImage(64, 48, 1);

  EncodeParams params;
  image_codec::offload::Outcome outcome;
  const auto encoded = router.encode(Format::Png, gray, params, &outcome);
  check(static_cast<bool>(encoded), "gray png via auto fallback");
  check(outcome.used == BackendKind::Cpu, "fell back to cpu");

  if (!outcome.notes.empty()) {
    std::printf("fallback note: %s\n", outcome.notes.front().c_str());
  }

  // 要么 OpenCL 没编译进来（note 说明 not built），要么它明确拒绝灰度图。
  check(!outcome.notes.empty(), "fallback recorded a reason");

  // 强制指定不支持的后端时必须失败，而不是悄悄换成别的后端。
  const auto forced =
      router.encodeWith(BackendKind::OpenCl, Format::Png, gray, params);
  const bool rejected = !forced;
  if (rejected) {
    std::printf("forced opencl on gray: %s\n", forced.error().message.c_str());
  }
  // OpenCL 未编译进来的构建里同样是「失败」，两种情形都算通过。
  check(rejected, "forced backend never silently swaps implementations");
}

}  // namespace

int main() {
  testBmpRoundtrip();
  testCpuRoundtrip();
  testGolden();
  testAnalytic();
  testBackendAgreement();
  testGrayChannelFidelity();
  testFallbackLeavesReason();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}
