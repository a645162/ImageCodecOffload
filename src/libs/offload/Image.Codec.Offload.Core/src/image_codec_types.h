/*
 * File: image_codec_types.h
 * Brief: Image codec types.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// image_codec_types.h - Image.Codec.Offload 公共类型（纯 C++，不依赖任何后端）。
//
// 这一层刻意不包含任何编解码实现：格式分派、后端选择、回退策略都在上层，
// 这里只定义「一张图是什么」「编码参数是什么」「失败怎么表达」。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string_view>
#include <string>
#include <vector>

namespace image_codec {
namespace offload {

// ---- 错误模型 ----
// 与三个后端 C API 的状态码一一对应（数值也对齐），上层可以把任何后端的
// 失败码直接映射过来，不需要为每个后端各写一张 switch 表。
enum class Status {
  Ok = 0,
  InvalidArgument = 1,
  OutOfMemory = 2,
  BackendUnavailable = 3,
  UnsupportedFormat = 4,
  DecodeFailed = 5,
  EncodeFailed = 6,
  Io = 7,
  Internal = 8,
};

const char *statusName(Status status);

// 失败原因必须能说人话：「显存预算不足：需要 25 MB，上限 8 MB」这类信息
// 写进日志，用户才知道该调什么；只回一个枚举值等于没说。
struct Error {
  Status code = Status::Internal;
  std::string message;

  Error() = default;
  Error(Status c, std::string m) : code(c), message(std::move(m)) {}
};

template <class T> using Result = std::expected<T, Error>;

// 便捷构造：`return fail<T>(Status::Io, "...")`。
template <class T>
inline std::unexpected<Error> fail(Status code, std::string message) {
  return std::unexpected<Error>(Error{code, std::move(message)});
}

// 容器格式。BMP 只作为调试用的中转格式存在（CLI 写出来看），
// 不作为编解码目标——三个后端都不承诺 BMP 的 GPU 路径。
enum class Format {
  Unknown = 0,
  Jpeg = 1,
  Tiff = 2,
  Png = 3,
  Jpeg2000 = 4,
  JpegXl = 5,
};

enum class CodecDirection {
  Decode = 0,
  Encode = 1,
};

const char *formatName(Format format);

struct FormatDescriptor {
  Format format = Format::Unknown;
  std::string_view displayName;
  std::string_view description;
  std::vector<std::string_view> extensions;
  std::vector<std::string_view> mimeTypes;
  uint64_t theoreticalMaxWidth = 0;
  uint64_t theoreticalMaxHeight = 0;
  uint64_t implementationMaxWidth = 0;
  uint64_t implementationMaxHeight = 0;
  bool decode = false;
  bool encode = false;
  bool metadata = false;
  bool tiled = false;
  bool decodeStreaming = false;
  bool encodeStreaming = false;
};

const std::vector<FormatDescriptor> &formatDescriptors();
const FormatDescriptor *formatDescriptor(Format format);
Format formatFromExtension(std::string_view extension);

// 从文件字节判断格式（看魔数，不看扩展名：扩展名错而内容对是最常见的现场情况）。
Format probeFormat(const uint8_t *data, size_t size);

// 计算后端。顺序即默认优先级（异构计算页可以改成别的顺序）。
enum class BackendKind {
  Cpu = 0,
  OpenCl = 1,
  Cuda = 2,
  Vulkan = 3,
};

const char *backendName(BackendKind kind);

// 编解码执行模式。能力页和任务报告必须区分 GPU 实际参与的阶段，不能把
// 通过 GPU 后端入口的 CPU 回退误报成 Native GPU。
enum class ExecutionMode {
  Unavailable = 0,
  Cpu = 1,
  HybridCpuGpu = 2,
  NativeGpu = 3,
  GpuStagingOnly = 4,
};

const char *executionModeName(ExecutionMode mode);

// 当前机器/设备在本次查询时可承载的保守像素上限。它不是格式标准的固定
// 宽高上限：显存会被其它进程占用，且可支持的长宽组合受 max-allocation
// 限制影响。因此用像素数表达，并明确给出用于计算的设备预算和每像素字节数。
// maxPixels == 0 表示后端无法提供可靠的动态上限，不表示没有上限。
struct RuntimeResolutionLimit {
  uint64_t maxPixels = 0;
  uint64_t availableBytes = 0;
  uint32_t bytesPerPixel = 0;
  std::string detail;
};

// 后端句柄的显存用量快照（诊断 / 矩阵报告用，不参与路由决策）。
//
// reported 单独存在是刻意的：`used_bytes == 0` 在报告里意味着「这一格确实
// 没占显存」，而「这个后端根本不跟踪单句柄占用」是另一个结论。两者混成
// 一个数字，报告里就会凭空长出一排漂亮的 0。
struct MemoryUsage {
  bool reported = false;
  // 设备总量与当前空闲（驱动视角，可能含其它进程占用；0 = 驱动/后端不提供）。
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  // 本句柄当前占用与其历史峰值（CUDA 真实统计；Vulkan 恒 0，见其 C 头说明）。
  uint64_t used_bytes = 0;
  uint64_t peak_bytes = 0;
  // reported == false 时的具体原因，或 reported == true 时的口径说明。
  std::string detail;
};

// ---- 图像 ----
// 统一用「紧凑、样本原样保留」表示：pixels 大小恒为 width*height*channels。
// channels 的取值与 PNG 颜色类型一致：1 = 灰度、2 = 灰度+Alpha、3 = RGB、
// 4 = RGBA；Alpha 是**非预乘**的原始样本，本层不做合成。
// 不提供 stride：追踪 stride 会把每个算子都变成两套下标，而我们的所有源
// （BMP 读盘、解码器输出、GPU 回读）本来都能产出紧凑布局。
struct Image {
  int width = 0;
  int height = 0;
  int channels = 0; // 1 / 2 / 3 / 4（见上）
  std::vector<uint8_t> pixels;

  bool valid() const {
    if (width <= 0 || height <= 0 || channels <= 0) return false;
    const size_t expected = static_cast<size_t>(width) *
                            static_cast<size_t>(height) *
                            static_cast<size_t>(channels);
    return pixels.size() == expected;
  }

  size_t rowBytes() const {
    return static_cast<size_t>(width) * static_cast<size_t>(channels);
  }
};

// JPEG 2000 的后缀既可能表示 JP2 容器，也可能表示裸 J2K 码流。
// JPF/JPX 仍使用 JP2 家族容器；该选择只影响编码，不影响统一的 Format 枚举。
enum class Jpeg2000Container {
  Jp2 = 0,
  J2k = 1,
};

// ---- 编码参数 ----
// 字段按目标格式选用；未用到的字段保持默认值即可。
// 大图调优出来的数字请写进调用方自己的配置（机器相关），不要改这里的默认值。
struct EncodeParams {
  Jpeg2000Container jpeg2000_container = Jpeg2000Container::Jp2;

  // JPEG
  int jpeg_quality = 90;      // 1..100
  int jpeg_subsampling = 420; // 444 / 422 / 420

  // JPEG 的其余编码控制。默认值刻意都是「关闭 / 跟随质量」：
  // 老调用方只填 quality + subsampling 时，产物与加入这些字段之前逐字节相同。
  bool jpeg_progressive = false;      // true = 渐进式（SOF2）
  bool jpeg_optimize_huffman = false; // true = 两遍扫描优化霍夫曼表
  int jpeg_restart_interval = 0;      // 0 = 不插 RST；>0 = 每 N 个 MCU
  int jpeg_luma_quality = -1;   // -1 = 跟随 jpeg_quality；0..100 = 单独指定
  int jpeg_chroma_quality = -1; // 同上（仅对 3 通道有意义）

  // TIFF
  int tiff_compression = 5;    // 1 = none, 5 = LZW, 7 = JPEG, 8 = deflate,
                               // 32773 = PackBits
  int tiff_tile_size = 0;      // 0 = 用 strip；>0 = tiled（边长，通常 256/512）
  int tiff_rows_per_strip = 0; // tiles=0 时生效；0 = 整幅图一条 strip
  // -1 = **不写 Predictor tag**（默认）。这不是「等于 PREDICTOR_NONE」的
  // 等价写法：值为 1 的 Predictor 标签也是合法的，但会多出一个 12 字节的
  // IFD 条目，于是「同参数不同路径」的产物差 12 字节。不写才是最中性的默认。
  int tiff_predictor = -1;      // 2 = horizontal，3 = floating point
  int tiff_x_dpi = 0;           // 0 = 不写分辨率标签
  int tiff_y_dpi = 0;           // 0 = 不写
  int tiff_resolution_unit = 0; // 0 = 有 DPI 时按英寸(2)；2 = 英寸；3 = 厘米
  int tiff_jpeg_quality = -1;   // -1 = 跟随 jpeg_quality（仅 compression=7）

  // PNG
  int png_compression = 6; // zlib 等级 0..9
  int png_filter = -1;     // -1 = 自适应；0..4 = 固定滤波器
  int png_strategy = -1;   // -1 = zlib 默认；0 default / 1 filtered / 2 huffman
                           // / 3 rle / 4 fixed（libpng 的 Z_*_STRATEGY）
};

// 一个后端**能表达**哪些编码参数。字段不是「值域」而是「这个开关有没有
// 实现」：例如 JPEG 的 restart_interval 只会问「支持吗」，具体间隔由参数给。
//
// 存在的理由：调用方（存图管线的 lane 过滤、UI 灰显）需要在**不做一次真实
// 编码**的前提下知道「这个后端会不会把我的参数丢掉」。没有这张表，GPU 后端
// 只能靠「编出来再看」发现问题，而那时文件已经写出来了。
struct EncodeCapabilities {
  bool jpeg_progressive = false;
  bool jpeg_optimize_huffman = false;
  bool jpeg_restart_interval = false;
  bool jpeg_luma_chroma_quality = false;
  bool tiff_predictor = false;
  bool tiff_resolution = false;
  bool tiff_packbits = false;
  bool tiff_jpeg_compression = false;
  bool png_strategy = false;
  // 后端能否把「按行区间」独立压缩成一条 strip 的压缩流（分片并行用）。
  //
  // 这个能力与「支持 TIFF」是两件事：整图编码只要写一个容器，分片编码还
  // 要求「同一段行、同样的标签、独立压缩」得到的字节与整图多 strip 编码的
  // 对应片逐位相同。做不到的后端必须报 false，由调度器退回整图单 lane，
  // 绝不允许用「分片结果差不多」蒙混——那会让文件里出现两种压缩参数的片。
  bool tiff_strip_sharding = false;
};

// SSIM 与 MS-SSIM 的结果。两者共享第 0 级统计，所以由一次调用一起给出。
struct SsimResult {
  double ssim = 0.0;
  double ms_ssim = 0.0;
  int levels = 0; // MS-SSIM 实际使用的尺度数（1..5）
};

// ---- 指标进度的回调 ----
//
// 大图上单是 SSIM 就要分钟级，界面必须能显示「算到哪了」。回调粒度是
// **一块（band）**：done/total 按像素数加权，单调递增，最后一次恒等于 total。
//
// 约束：
//   - 在**累加线程**上调用（不是任意 worker），调用方不需要自己加锁；
//   - 必须足够便宜：界面侧只在百分比变化时才投递事件，不要在回调里做 IO；
//   - 不是所有后端都提供：GPU 后端一次 kernel 下发，没有中间反馈点，它们
//     不会调用这个回调（宁可没有进度，也不报一个假进度）。
using MetricsProgressFn = std::function<void(int done, int total)>;

} // namespace offload
} // namespace image_codec
