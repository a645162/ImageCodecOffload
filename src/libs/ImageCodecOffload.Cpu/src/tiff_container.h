/*
 * File: tiff_container.h
 * Brief: classic TIFF 容器的拼装（分片并行用）：头 + strip 压缩流 + IFD。
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-11-01
 */

// tiff_container.h - 把「已经压好的 strip 压缩流」拼成一个 classic TIFF。
//
// 为什么不能继续用 libtiff 拼：`TIFFWriteRawStrip()` 是**更新模式**的 API，
// 新建文件时 StripOffsets 要等写目录（TIFFClose）才定下来，raw 写入落不到
// 正确位置——实测拼出来的文件只有 152 字节（头 + IFD，一条 strip 数据都
// 没有）。因此分片方案必须自己写容器。
//
// 为什么自己写还能与 libtiff 的产物**逐字节相同**：TIFF 的目录布局虽然被
// 规范约束了语义，但「条目顺序 / 额外值顺序 / 数组类型」都是实现细节。
// 这里逐条复刻了 libtiff 4.7.2 的做法（见各处的注释），并在 tests/ico_test.cpp
// 里用「整图多 strip 编码」的产物做字节级对拍。**改动这段代码必须同时跑那个
// 对拍**，否则升级 libtiff 后会以「文件能打开、像素也对、只有校验和变了」
// 的形式悄悄失配。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "image_codec_types.h"

namespace image_codec {
namespace offload {
namespace cpu {

// 拼装一份 TIFF 需要的**全部**容器级信息。压缩流由各 lane 各自产出，
// 拼装只负责把它们按顺序放进文件并写出目录。
struct TiffLayout {
  int width = 0;
  int height = 0;
  int channels = 1;    // 1 / 2 / 3 / 4（chunky，不写 planar=2）
  int compression = 1; // TIFF Compression 标签值（1/5/7/8/32773）
  int predictor = -1;  // -1 = 不写 Predictor 标签（与整图编码一致）
  int x_dpi = 0;       // 0 = 不写分辨率标签
  int y_dpi = 0;
  int resolution_unit = 0; // 2 = 英寸（默认）、3 = 厘米
  int rows_per_strip = 0;  // > 0；分片时就是片高
};

// 拼装。strips 必须按行顺序给出，且条数 == ceil(height / rows_per_strip)。
//
// 失败原因（条数不符、超出 classic TIFF 的 4 GiB 偏移、参数非法）都通过
// Status 表达，不允许返回一个「看起来能打开」的残缺文件。
Result<std::vector<uint8_t>>
assembleClassicTiff(const TiffLayout& layout,
                    const std::vector<std::vector<uint8_t>> &strips);

// 这个压缩方式是否**接受** Predictor 标签（LZW / Deflate / AdobeDeflate）。
//
// 判定必须与 libtiff 一致：Predictor 字段是由预测器编解码器（tif_predict.c
// 的 TIFFPredictorInit）在**注册压缩算法时**才合并进字段表的，所以 NONE 与
// PackBits 下 `TIFFSetField(TIFFTAG_PREDICTOR, ...)` 会打出
// "Unknown tag 317" 并且**什么都不写**。容器拼装若照参数照写标签，就会比
// 整图编码多出 12 字节的 IFD 条目——文件合法、像素相同、校验和不同。
bool compressionSupportsPredictor(int compression) noexcept;

// 这个压缩方式能否参与分片（NONE / LZW / Deflate / PackBits）。
// JPEG-in-TIFF 与 tiled 布局都**不支持**分片：前者要靠 TIFFTAG_JPEGTABLES
// 共享量化表，后者是 Tile* 数组而不是 Strip* 数组，拼装规则完全不同。
bool compressionSupportsStripSharding(int compression) noexcept;

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
