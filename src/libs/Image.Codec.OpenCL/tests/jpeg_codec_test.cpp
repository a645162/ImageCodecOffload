/*
 * File: jpeg_codec_test.cpp
 * Brief: Jpeg codec test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// jpeg_codec_test.cpp - JPEG 混合编解码自检（GPU DCT/颜色变换 + CPU Huffman）。
//
// 检查三件最容易悄悄错、而且「能编码能解码」完全掩盖得住的事：
//   1. 子采样与容器自洽：4:2:2 的 SOF 必须是 h2v1（h_samp=2, v_samp=1，max_v=1），
//      MCU 网格按 8 行算。把下采样写死 2x2、或者让 max_v 沿用 4:2:0 的 2，
//      文件仍然打得开、亮度也正常，只是色度整幅错位——只有平滑渐变图看得出来。
//   2. 通道保真：灰度进必须单分量出，绝不悄悄升维成 3 通道。
//   3. GPU 解码与 CPU 参考解码在同一份码流上给出同一幅图（不是「都挺像」）。
//
// 退出码：0 = 通过，2 = SKIP（没有 OpenCL 设备），其它 = 失败。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "jpeg_bitstream.h"
#include "jpeg_codec.h"

using image_codec::jpeg::JpegDecodeResult;
using image_codec::jpeg::JpegImage;

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

// 有局部随机性的确定性图案：与三个后端的同名测试同 seed、同公式，
// 便于跨后端比对同一张图上的数字。
std::vector<uint8_t> makePattern(int w, int h, int c) {
  std::vector<uint8_t> data(static_cast<size_t>(w) * h * c);
  std::mt19937 rng(12345);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (int ch = 0; ch < c; ++ch) {
        const size_t idx = (static_cast<size_t>(y) * w + x) * c + ch;
        data[idx] = static_cast<uint8_t>(
            (x * 3 + y * 5 + ch * 37 + static_cast<int>(rng() % 7)) & 0xFF);
      }
    }
  }
  return data;
}

// 平滑渐变（不跨 0xFF 回绕）：随机图案上色度本身就是噪声，行取错也只差几个
// dB；只有垂直方向有明显色彩变化的图才能暴露 4:2:2 的行错位。
std::vector<uint8_t> makeGradient(int w, int h) {
  std::vector<uint8_t> data(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      data[idx] = static_cast<uint8_t>((x * 2) & 0xFF);
      data[idx + 1] = static_cast<uint8_t>((y * 4) & 0xFF);
      data[idx + 2] = static_cast<uint8_t>(((x + y)) & 0xFF);
    }
  }
  return data;
}

// 宿主 PSNR：单层遍历（不做嵌套循环）。它只做质量闸门，不与指标模块的
// golden 常量争主——指标另有 metrics_test 专门对拍。
double hostPsnr(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
  if (a.size() != b.size() || a.empty()) return -1.0;
  double sum = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  const double mse = sum / static_cast<double>(a.size());
  if (mse <= 0.0) return 99.0;
  return 10.0 * std::log10((255.0 * 255.0) / mse);
}

bool encodeJpeg(const std::vector<uint8_t> &pixels, int w, int h, int channels,
                int quality, int subsampling, std::vector<uint8_t> *out) {
  std::string err;
  if (image_codec::jpeg::jpeg_encode_buffer(pixels, w, h, channels, quality,
                                            subsampling, /*backend=*/0, out,
                                            &err) != 0) {
    check(false, "encode failed: " + err);
    return false;
  }
  return true;
}

bool decodeJpeg(const std::vector<uint8_t> &file, int backend,
                JpegDecodeResult *out) {
  if (image_codec::jpeg::jpeg_decode_buffer(file, backend, out) != 0) {
    check(false, std::string("decode failed (backend ") +
                     std::to_string(backend) + "): " + out->error);
    return false;
  }
  return true;
}

// 一次性跑「编码 -> 解析 -> GPU 解码 -> CPU 参考解码」，返回 GPU 解码的 PSNR。
// 顺带断言容器里的采样因子与 MCU 网格——那是子采样缺陷唯一直接可见的证据。
struct CaseResult {
  bool ok = false;
  double psnr_gpu = -1.0;
  double psnr_cpu = -1.0;
};

CaseResult runCase(const std::vector<uint8_t> &pixels, int w, int h,
                   int channels, int subsampling, int expect_h_samp,
                   int expect_v_samp, const std::string &tag) {
  CaseResult result;
  std::vector<uint8_t> jpeg;
  if (!encodeJpeg(pixels, w, h, channels, 90, subsampling, &jpeg))
    return result;

  // 容器的采样因子必须与请求一致：能解码但 SOF 写错时，解码器会按错的形状
  // 回读，颜色整体跑偏。
  JpegImage parsed;
  std::string parse_err;
  if (image_codec::jpeg::jpeg_parse(jpeg, &parsed, &parse_err) != 0) {
    check(false, tag + ": parse failed: " + parse_err);
    return result;
  }
  check(parsed.ncomp == channels, tag + ": component count matches channels");
  check(parsed.comp[0].h_samp == expect_h_samp &&
            parsed.comp[0].v_samp == expect_v_samp,
        tag + ": luma sampling is " + std::to_string(parsed.comp[0].h_samp) +
            "x" + std::to_string(parsed.comp[0].v_samp) + ", want " +
            std::to_string(expect_h_samp) + "x" +
            std::to_string(expect_v_samp));

  // MCU 网格由 SOF 的采样因子推回，是「色度平面高度」的另一种写法：4:2:2
  // 必须是 8 行一个 MCU。这条断言挡的是「SOF 与扫描写法互相矛盾」的码流
  // （解码器会按错的行数回读），但看不出编码器内部的 max_v 用错——那是
  // 往返 PSNR 才抓得到的，所以下面还要真解码一次。
  const int expect_mcus_y = (h + (8 * expect_v_samp) - 1) / (8 * expect_v_samp);
  check(parsed.max_v == expect_v_samp,
        tag + ": max_v == " + std::to_string(expect_v_samp));
  check(parsed.mcus_y == expect_mcus_y,
        tag + ": mcus_y == " + std::to_string(expect_mcus_y) + " (got " +
            std::to_string(parsed.mcus_y) + ")");

  JpegDecodeResult gpu;
  if (!decodeJpeg(jpeg, /*backend=*/0, &gpu)) return result;
  check(gpu.width == w && gpu.height == h, tag + ": geometry");
  // 通道保真：1 分量进必须 1 通道出，绝不升维。
  check(gpu.channels == channels, tag + ": channel fidelity");
  check(gpu.rgb.size() == static_cast<size_t>(w) * h * channels,
        tag + ": decoded buffer size");
  result.psnr_gpu = hostPsnr(pixels, gpu.rgb);

  JpegDecodeResult cpu;
  if (!decodeJpeg(jpeg, /*backend=*/1, &cpu)) return result;
  check(cpu.channels == channels, tag + ": cpu channel fidelity");
  result.psnr_cpu = hostPsnr(pixels, cpu.rgb);

  // GPU 与 CPU 参考解码必须落在同一幅图上：IDCT/颜色变换的定点与浮点差异
  // 允许存在，但不允许「一个对、一个错」。
  check(std::fabs(result.psnr_gpu - result.psnr_cpu) <= 0.5,
        tag + ": gpu/cpu decode agree (gpu " + std::to_string(result.psnr_gpu) +
            " dB vs cpu " + std::to_string(result.psnr_cpu) + " dB)");
  result.ok = true;
  return result;
}

} // namespace

int main() {
  // 先探测设备：没有 OpenCL 设备时明确 SKIP，而不是报一堆假失败。
  const std::vector<uint8_t> tiny = makePattern(8, 8, 1);
  std::string probe_err;
  std::vector<uint8_t> probe_jpeg;
  if (image_codec::jpeg::jpeg_encode_buffer(tiny, 8, 8, 1, 90, 444,
                                            /*backend=*/0, &probe_jpeg,
                                            &probe_err) != 0) {
    std::printf("SKIP: %s\n", probe_err.c_str());
    return 2;
  }

  // ---- 1. 灰度：必须单分量出，绝不升维 ----
  {
    const std::vector<uint8_t> gray = makePattern(65, 39, 1);
    const CaseResult r = runCase(gray, 65, 39, 1, 444, 1, 1, "gray 65x39");
    check(r.ok, "gray: encode/decode succeeded");
    check(r.psnr_gpu >= 35.0,
          "gray: psnr >= 35 (got " + std::to_string(r.psnr_gpu) + ")");
    std::printf("  %-22s psnr=%.2f dB\n", "gray q90 4:4:4", r.psnr_gpu);
  }

  // ---- 2. RGB 4:4:4：无色度损失，随机图案也应明显好于 4:2:0 ----
  double psnr_444 = -1.0;
  {
    const std::vector<uint8_t> rgb = makePattern(65, 39, 3);
    const CaseResult r = runCase(rgb, 65, 39, 3, 444, 1, 1, "rgb 65x39 444");
    check(r.ok, "rgb 444: encode/decode succeeded");
    check(r.psnr_gpu >= 30.0,
          "rgb 444: psnr >= 30 (got " + std::to_string(r.psnr_gpu) + ")");
    psnr_444 = r.psnr_gpu;
    std::printf("  %-22s psnr=%.2f dB\n", "rgb q90 4:4:4", r.psnr_gpu);
  }

  // ---- 3. RGB 4:2:0 ----
  {
    const std::vector<uint8_t> rgb = makePattern(65, 39, 3);
    const CaseResult r = runCase(rgb, 65, 39, 3, 420, 2, 2, "rgb 65x39 420");
    check(r.ok, "rgb 420: encode/decode succeeded");
    check(r.psnr_gpu >= 18.0,
          "rgb 420: psnr >= 18 (got " + std::to_string(r.psnr_gpu) + ")");
    // 色度真的被丢掉了：随机图案上 4:2:0 必须明显差于 4:4:4，否则说明
    // subsampling 参数根本没生效。
    check(r.psnr_gpu < psnr_444, "rgb 420: chroma actually subsampled (420 " +
                                     std::to_string(r.psnr_gpu) + " dB < 444 " +
                                     std::to_string(psnr_444) + " dB)");
    std::printf("  %-22s psnr=%.2f dB\n", "rgb q90 4:2:0", r.psnr_gpu);
  }

  // ---- 4. RGB 4:2:2：本条是本轮修掉的缺陷的回归测试 ----
  {
    const std::vector<uint8_t> grad = makeGradient(65, 39);
    const CaseResult r422 =
        runCase(grad, 65, 39, 3, 422, 2, 1, "gradient 65x39 422");
    const CaseResult r420 =
        runCase(grad, 65, 39, 3, 420, 2, 2, "gradient 65x39 420");
    check(r422.ok, "gradient 422: encode/decode succeeded");
    check(r420.ok, "gradient 420: encode/decode succeeded");

    // 绝对下限：写死 2x2 时这张图只有 19 dB 上下，整幅错位时 15 dB 上下。
    check(r422.psnr_gpu >= 30.0, "gradient 422: psnr >= 30 (got " +
                                     std::to_string(r422.psnr_gpu) + ")");
    // 相对关系：平滑图上 4:2:2 只丢水平色度，理应不比 4:2:0 差。
    check(r422.psnr_gpu >= r420.psnr_gpu - 1.0,
          "gradient 422: not worse than 420 (422 " +
              std::to_string(r422.psnr_gpu) + " dB vs 420 " +
              std::to_string(r420.psnr_gpu) + " dB)");
    std::printf("  %-22s psnr=%.2f dB (4:2:0 %.2f dB)\n", "gradient q90 4:2:2",
                r422.psnr_gpu, r420.psnr_gpu);
  }

  // ---- 5. 非法参数必须被拒绝，不能悄悄降维 ----
  {
    std::vector<uint8_t> out;
    std::string err;
    const std::vector<uint8_t> px = makePattern(16, 16, 2);
    check(image_codec::jpeg::jpeg_encode_buffer(px, 16, 16, 2, 90, 420,
                                                /*backend=*/0, &out, &err) != 0,
          "encode: 2 channels rejected (jpeg has no alpha)");
    check(image_codec::jpeg::jpeg_encode_buffer(px, 16, 16, 4, 90, 420,
                                                /*backend=*/0, &out, &err) != 0,
          "encode: 4 channels rejected (jpeg has no alpha)");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}
