// File      : tiff_predict_vk.h
// Brief     : TIFF Predictor=2（水平差分）的 Vulkan 实现与 CPU 参考
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 为什么只有预测器在 GPU：TIFF 的 LZW 字典构建是严格串行的（下一个码依赖
// 上一个码写进字典），搬到 GPU 只会得到一条「看起来很并行、实际比 CPU 慢」
// 的路径。Predictor=2 的差分**不跨行**，一个 band 内每行独立，正好适合 GPU。
//
// 输入输出都是「rows 个独立的行」连续排布（行宽 row_bytes）：
//   * strip 布局：一张图连续的若干行；
//   * tiled 布局：一批 tile 的 padded 行——因为 tile 行宽一致，可以当成
//     「n 个 tile × tile_h 行」的一整块来算，不必一个 tile 派发一次。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vk_buffer.h"
#include "vk_codec_device.h"
#include "vk_pipeline.h"

namespace image_codec {
namespace vulkan {
namespace tiff {

// CPU 参考：out[i] = in[i] - in[i-channels]（每行前 channels 个字节的左邻为 0）。
void predictRowsCpu(const uint8_t *src, size_t row_bytes, int channels,
                    int rows, uint8_t *out);

// 原地版本。不能直接用 predictRowsCpu(src == dst)：左邻字节会先被覆盖。
void predictRowsCpuInPlace(uint8_t *data, size_t row_bytes, int channels,
                           int rows);

// CPU 参考：预测还原（原地可用）。s[i] = d[i] + s[i-channels]。
void unpredictRowsCpu(uint8_t *data, size_t row_bytes, int channels, int rows);

// GPU 预测器。构造后 init() 一次，run() 可反复调用（缓冲按需增长）。
// 失败原因一定写明是设备不可用、显存不足还是派发失败——上层据此决定
// 「换后端」还是「回退 CPU」，而不是只看得到一个 false。
class PredictorGpu {
public:
  PredictorGpu() = default;
  ~PredictorGpu() = default;
  PredictorGpu(const PredictorGpu &) = delete;
  PredictorGpu &operator=(const PredictorGpu &) = delete;

  // memory_limit == 0 表示不限制（仍受设备真实空闲显存约束）。
  // device_index < 0 = 自动选（独显优先）。
  bool init(size_t memory_limit, int device_index, std::string *error);

  bool ready() const { return ready_; }
  const std::string &lastError() const { return error_; }

  // 把 data 里的 rows 个行原地替换成水平差分结果。
  // band_rows <= 0 时按 256 行一批；每批字节数另有 64 MB 上限，
  // 以免超大行宽把 int 推到溢出。
  bool run(uint8_t *data, size_t row_bytes, int channels, int rows,
           int band_rows, std::string *error);

private:
  Device device_;
  Pipeline pipeline_;
  Buffer src_;
  Buffer dst_;
  std::vector<uint32_t> words_;
  std::string error_;
  bool ready_ = false;
};

} // namespace tiff
} // namespace vulkan
} // namespace image_codec
