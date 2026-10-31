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

#include <ImageCodecOffload/image_codec_offload.h>

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

void check(bool ok, const char *what) {
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

} // namespace

int main() {
  testBmpRoundtrip();
  testCpuRoundtrip();
  testCpuChannelMatrix();
  testJpegAlphaRejected();
  testFourChannelCrossBackend();
  testBackendRoundtrip();
  testGolden();
  testAnalytic();
  testBackendAgreement();
  testGrayChannelFidelity();
  testFallbackLeavesReason();
  testForcedBackendApi();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}
