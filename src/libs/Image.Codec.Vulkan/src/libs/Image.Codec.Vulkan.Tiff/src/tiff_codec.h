// File      : tiff_codec.h
// Brief     : TIFF 编解码门面（容器 + LZW；预测器可在 GPU，LZW 在 CPU）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 分工：
//   * Predictor=2（水平差分）在 Vulkan compute——逐行独立，正是 GPU 该干的活；
//   * LZW 编解码在 CPU——字典条目依赖前一条输出，严格串行，GPU 版本在这类
//     8-bit chunky 数据上一直慢于标量 CPU（Image.Codec.OpenCL 的早期结论一致）。
// 因此解码整条链路都在 CPU，不存在「看着有 GPU 参与、其实更慢」的路径。
//
// 预测器只是压缩前的可逆变换：开不开都不影响解码结果，只影响压缩率。
// 关闭它等于让 GPU 无事可做，但产物同样是合法 TIFF。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "image_buffer.h"

namespace image_codec {
namespace vulkan {
namespace tiff {

struct TiffEncodeOptions {
  int compression = 5;    // 1 = none, 5 = LZW
  int rows_per_strip = 0; // 0 = 自动（64 行，且整除高度）
  int tile_size = 0;      // >0 = tiled 布局（必须是 16 的倍数）

  // 是否让 GPU 做 Predictor=2。false 时用 CPU 参考实现，产物逐字节相同，
  // 只是没有加速——这使「设备不支持 fp64」不至于让 TIFF 完全不可用。
  bool use_gpu = true;
  // 是否写 Predictor=2（仅 LZW 有意义）。关闭时 GPU 无事可做，gpu_used=false。
  bool predictor = true;

  // GPU 每批行数（显存占用只与它和行宽有关，与图像高度无关）。
  int band_rows = 256;
  // 显存上限（0 = 不限制，仍受设备真实空闲显存约束）。
  size_t memory_limit = 0;
  // 设备选择：-1 = 自动（独显优先），>=0 = 指定下标。
  int device_index = -1;
  // GPU 不可用/显存不足时是否回退 CPU 预测器。回退产出的字节完全一致，
  // 因此默认允许；gpu_used 会如实报 false。
  bool allow_cpu_fallback = true;
};

// 编码为 TIFF 字节流。gpu_used（可为 nullptr）回报预测器是否真的在 GPU 上算过。
bool encodeTiff(const ImageBuffer &img, const TiffEncodeOptions &options,
                std::vector<uint8_t> *out, bool *gpu_used, std::string *err);

// 探测魔数（II*\0 / MM\0*）。上层据此刻画「这张 buffer 是不是 TIFF」，
// 不必先尝试解码一次。
inline bool hasTiffSignature(const uint8_t *p, size_t n) {
  if (p == nullptr || n < 4) return false;
  const bool le = p[0] == 'I' && p[1] == 'I' && p[2] == 42 && p[3] == 0;
  const bool be = p[0] == 'M' && p[1] == 'M' && p[2] == 0 && p[3] == 42;
  return le || be;
}

// 解码 TIFF 字节流（8-bit chunky；compression 1/5；strip 与 tiled 均可）。
bool decodeTiff(const uint8_t *data, size_t size, ImageBuffer *out,
                std::string *err);

} // namespace tiff
} // namespace vulkan
} // namespace image_codec
