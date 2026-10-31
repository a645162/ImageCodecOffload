// image_codec_types.h - ImageCodecOffload 公共类型（纯 C++，不依赖任何后端）。
//
// 这一层刻意不包含任何编解码实现：格式分派、后端选择、回退策略都在上层，
// 这里只定义「一张图是什么」「编码参数是什么」「失败怎么表达」。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
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

const char* statusName(Status status);

// 失败原因必须能说人话：「显存预算不足：需要 25 MB，上限 8 MB」这类信息
// 写进日志，用户才知道该调什么；只回一个枚举值等于没说。
struct Error {
  Status code = Status::Internal;
  std::string message;

  Error() = default;
  Error(Status c, std::string m) : code(c), message(std::move(m)) {}
};

template <class T>
using Result = std::expected<T, Error>;

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
};

const char* formatName(Format format);

// 从文件字节判断格式（看魔数，不看扩展名：扩展名错而内容对是最常见的现场情况）。
Format probeFormat(const uint8_t* data, size_t size);

// 计算后端。顺序即默认优先级（异构计算页可以改成别的顺序）。
enum class BackendKind {
  Cpu = 0,
  OpenCl = 1,
  Cuda = 2,
  Vulkan = 3,
};

const char* backendName(BackendKind kind);

// ---- 图像 ----
// 统一用「紧凑 RGB / 灰度」表示：pixels 大小恒为 width*height*channels。
// 不提供 stride：追踪 stride 会把每个算子都变成两套下标，而我们的所有源
// （BMP 读盘、解码器输出、GPU 回读）本来都能产出紧凑布局。
struct Image {
  int width = 0;
  int height = 0;
  int channels = 0;  // 1（灰度）或 3（RGB）
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

// ---- 编码参数 ----
// 字段按目标格式选用；未用到的字段保持默认值即可。
// 大图调优出来的数字请写进调用方自己的配置（机器相关），不要改这里的默认值。
struct EncodeParams {
  // JPEG
  int jpeg_quality = 90;       // 1..100
  int jpeg_subsampling = 420;  // 444 / 422 / 420

  // TIFF
  int tiff_compression = 5;      // 1 = none, 5 = LZW, 8 = deflate
  int tiff_tile_size = 0;        // 0 = 用 strip；>0 = tiled（边长，通常 256/512）
  int tiff_rows_per_strip = 0;   // tiles=0 时生效；0 = 按显存/内存预算自动

  // PNG
  int png_compression = 6;  // zlib 等级 0..9
  int png_filter = -1;      // -1 = 自适应；0..4 = 固定滤波器
};

// SSIM 与 MS-SSIM 的结果。两者共享第 0 级统计，所以由一次调用一起给出。
struct SsimResult {
  double ssim = 0.0;
  double ms_ssim = 0.0;
  int levels = 0;  // MS-SSIM 实际使用的尺度数（1..5）
};

}  // namespace offload
}  // namespace image_codec
