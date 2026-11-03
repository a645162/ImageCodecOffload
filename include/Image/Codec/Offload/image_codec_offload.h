/*
 * File: image_codec_offload.h
 * Brief: Image codec offload.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// image_codec_offload.h - Image.Codec.Offload 对外 C API（纯 C，C11）。
//
// 这一层是给**外部程序**（ImageViewer、批处理工具、C# 互操作）用的：它只面向
// 「格式 + 像素 + 参数」，不暴露任何后端类型。调用方不需要知道本机有 OpenCL
// 还是有 CUDA，也不需要知道某个格式在当前后端能不能用。
//
// 用法：
//   #include <Image/Codec/Offload/image_codec_offload.h>
//   ico_codec* codec = NULL;
//   image_codec_offload_create(&codec, IMAGECODEC_OFFLOAD_BACKEND_AUTO);
//   ... image_codec_offload_encode / decode / metric_* ...
//   image_codec_offload_destroy(codec);
//
// 线程安全：本 API 内部不加锁；同一句柄并发调用需调用方同步。
// 所有函数返回 ico_status，0 表示成功。
#pragma once

#include <stddef.h>
#include <stdint.h>

// ---- 导出宏 ----
#if defined(_WIN32) && defined(IMAGECODEC_OFFLOAD_BUILD_SHARED)
#if defined(IMAGECODEC_OFFLOAD_BUILDING_LIB)
#define ICO_API __declspec(dllexport)
#else
#define ICO_API __declspec(dllimport)
#endif
#else
#define ICO_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- 状态码 ----
// 数值与三个后端子项目（Image.Codec.OpenCL / Image.Codec.CUDA / Image.Codec.Vulkan）
// 一致，上层可以把任何后端的失败码直接映射过来。
typedef enum ico_status {
  ICO_OK = 0,                      // 成功
  ICO_ERR_INVALID_ARGUMENT = 1,    // 参数非法（空指针/越界/不支持的组合）
  ICO_ERR_OUT_OF_MEMORY = 2,       // 内存/显存不足（含输出缓冲过小）
  ICO_ERR_BACKEND_UNAVAILABLE = 3, // 指定后端不可用（未编译进来/无设备）
  ICO_ERR_UNSUPPORTED_FORMAT = 4,  // 不支持的格式/方向/通道数
  ICO_ERR_DECODE_FAILED = 5,       // 解码失败（码流损坏等）
  ICO_ERR_ENCODE_FAILED = 6,       // 编码失败
  ICO_ERR_IO = 7,                  // 文件 I/O 错误
  ICO_ERR_INTERNAL = 8             // 内部错误
} ico_status;

// 容器格式。BMP 只作为 CLI/测试的中转格式，不是本库的编解码目标。
typedef enum ico_format {
  ICO_FORMAT_JPEG = 0,
  ICO_FORMAT_TIFF = 1,
  ICO_FORMAT_PNG = 2,
  ICO_FORMAT_JPEG2000 = 3,
  ICO_FORMAT_JPEG_XL = 4,
  ICO_FORMAT_UNKNOWN = -1
} ico_format;

// 计算后端。数值与内部 BackendKind 一致；AUTO 表示「按优先级自动选 + 回退」。
typedef enum ico_backend {
  ICO_BACKEND_AUTO = -1,
  ICO_BACKEND_CPU = 0,
  ICO_BACKEND_OPENCL = 1,
  ICO_BACKEND_CUDA = 2,
  ICO_BACKEND_VULKAN = 3
} ico_backend;

typedef enum ico_execution_mode {
  ICO_EXECUTION_UNAVAILABLE = 0,
  ICO_EXECUTION_CPU = 1,
  ICO_EXECUTION_HYBRID_CPU_GPU = 2,
  ICO_EXECUTION_NATIVE_GPU = 3,
  /* GPU performed data staging only; no codec pixel stage ran on the device. */
  ICO_EXECUTION_GPU_STAGING_ONLY = 4
} ico_execution_mode;

// JPEG 2000 输出容器。JP2/JPF/JPX 使用 ICO_JPEG2000_CONTAINER_JP2；
// 只有 .j2k 使用裸码流。
typedef enum ico_jpeg2000_container {
  ICO_JPEG2000_CONTAINER_JP2 = 0,
  ICO_JPEG2000_CONTAINER_J2K = 1
} ico_jpeg2000_container;

// 解码产物 / 编码输入。
//   channels：1 = 灰度、2 = 灰度+Alpha、3 = RGB、4 = RGBA（Alpha 非预乘）。
//   与 PNG 的四种颜色类型一一对应；PNG / TIFF 全支持，JPEG 没有 Alpha 样本，
//   2/4 通道会返回 ICO_STATUS_INVALID_ARGUMENT（不静默降维）。
//   data 为 top-down 行序（第 0 行为图像顶部）；stride == 0 时按 width * channels 处理。
// 解码输出由本库分配，调用方用 image_codec_offload_image_free 释放。
typedef struct ico_image {
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint8_t *data;
  size_t stride;    // 每行字节数
  size_t data_size; // 像素数据总字节数
} ico_image;

// 行流式编码的调用方回调。destination 始终是恰好
// width * channels 字节的 top-down 紧凑行；返回非 ICO_OK 会立即停止编码。
typedef ico_status (*ico_row_reader)(void *user, int y, uint8_t *destination,
                                     size_t destination_size);

// 编码参数。字段按目标格式选用，未用到的字段忽略（保持 0 即用默认值）。
//
// 「未填」的约定统一是 **0 或负数**，因为 0 在多数参数里是一个合法取值
// （质量不能是 0，但 rows_per_strip=0、filter=0、predictor=0 都要能表达）。
// 具体见每个字段的注释。
typedef struct ico_encode_params {
  // JPEG
  int jpeg_quality;     // 1..100，0 = 默认 90
  int jpeg_subsampling; // 0 = 默认(420)，444/422/420
  int jpeg_progressive;      // 0 = baseline(SOF0)；1 = 渐进式(SOF2)
  int jpeg_optimize_huffman; // 0 = 一遍扫描；1 = 两遍，优化霍夫曼表
  int jpeg_restart_interval; // 0 = 不插 RST；>0 = 每 N 个 MCU 一个 RST
  int jpeg_luma_quality;     // 0 = 跟随 jpeg_quality；1..100 = 单独指定
  int jpeg_chroma_quality;   // 0 = 跟随 jpeg_quality；1..100 = 单独指定

  // TIFF
  int tiff_compression;    // 0 = 默认 LZW；1 none / 5 LZW / 7 JPEG /
                           // 8 AdobeDeflate / 32773 PackBits
  int tiff_rows_per_strip; // 0 = 整幅图一条 strip（与 libtiff 写 scanline 的
                           // 默认结果一致）；>0 = 显式行数
  int tiff_tile_size;      // 0 = strip 布局；>0 = tiled（16 的倍数）
  // -1 或 0 = **不写 Predictor 标签**（默认）。1 = none、2 = horizontal、
  // 3 = floating point。注意「不写」与「写 1」像素等价，但后者会多一个
  // IFD 条目（12 字节），逐字节对拍时会看成差异。
  int tiff_predictor;
  int tiff_x_dpi;           // 0 = 不写分辨率标签
  int tiff_y_dpi;           // 0 = 不写（x 给了、y 没给时 y 跟随 x）
  int tiff_resolution_unit; // 0 = 有 DPI 时按英寸；2 = 英寸；3 = 厘米
  int tiff_jpeg_quality;    // 0 = 跟随 jpeg_quality；1..100（仅压缩=7）

  // PNG
  // **这两个字段的「未指定」是 -1，不是 0**：zlib 等级 0 与「不滤波」都是
  // 合法取值，用 0 表示未指定会让它们永远表达不出来（曾经就是这样）。
  int png_compression; // -1 = 默认 6；0..9 = 显式等级
  int png_filter;      // -1 = 自适应；0 None / 1 Sub / 2 Up / 3 Avg / 4 Paeth
  int png_strategy;    // 0 = 未指定（zlib 默认）；1 filtered / 2 huffman /
                       // 3 rle / 4 fixed
} ico_encode_params;

// 某个句柄**能表达**哪些编码参数。字段是「有没有实现」而不是「值域」。
//
// 为什么需要它：一条存图 lane 绑死一个后端，调用方必须在派发前知道这组
// 参数在这个后端上会不会被丢掉。没有这张表就只能「编一张试试」，而失败
// 发生在文件写出来之前、大图上还白跑一遍。0 = 不支持，非 0 = 支持。
typedef struct ico_encode_capabilities {
  int jpeg_progressive;
  int jpeg_optimize_huffman;
  int jpeg_restart_interval;
  int jpeg_luma_chroma_quality;
  int tiff_predictor;
  int tiff_resolution;
  int tiff_packbits;
  int tiff_jpeg_compression;
  int png_strategy;
  // 能否把「一段连续行」独立压缩成一条 TIFF strip 的压缩流（见
  // image_codec_offload_tiff_strip_encode）。分片并行只在这个能力为真时可用。
  int tiff_strip_sharding;
} ico_encode_capabilities;

// SSIM 与 MS-SSIM 的结果：两者共享第 0 级统计，由一次调用一起给出。
typedef struct ico_ssim {
  double ssim;
  double ms_ssim;
  int levels; // MS-SSIM 实际使用的尺度数（1..5）
} ico_ssim;

// 后端的一次探测结果。
typedef struct ico_backend_info {
  int backend;      // ico_backend 的整数值
  int ready;        // 1 = 可用
  char detail[256]; // ready 时为 "ready"，否则是具体原因
} ico_backend_info;

// 不透明句柄。
typedef struct ico_codec ico_codec;

// 状态码 -> 可读字符串（静态存储，勿 free）。
ICO_API const char *image_codec_offload_status_string(ico_status status);

// 创建编解码器。preferred_backend 为 ICO_BACKEND_AUTO 时使用默认优先级
// （CUDA → OpenCL → CPU），否则把它排到第一位、其余按默认顺序兜底。
ICO_API ico_status image_codec_offload_create(ico_codec **out_codec,
                                              int preferred_backend);

// 创建**强制**后端的编解码器：只使用该后端，失败即失败，绝不回退。
//
// 与 create()/set_order() 的差别只有「回退」这一件事，但缺了它就表达不了
// 「这条 lane 只用 CPU」：set_order 会把 CPU 排到最后（兜底不能丢），于是
// 首选 CPU 时实际仍先跑 GPU。多个 lane 并发编码时这会让 CPU lane 与 GPU lane
// 变成同一个设备上的两条 lane：抢显存、统计混在一起、A/B 对比无从谈起。
//
// 使用场景：一条 lane = 一个后端（存图管线的 CPU/GPU 同时调度）。
// 单次调用的场景仍应使用 create()，那里回退是优点。
ICO_API ico_status image_codec_offload_create_forced(ico_codec **out_codec,
                                                     int backend);

// 销毁。NULL 安全。
ICO_API void image_codec_offload_destroy(ico_codec *codec);

// 设置后端优先级（kinds 为 ico_backend 的整数值数组）。CPU 永远被追加到最后，
// 即使调用方没写它——配置错误不该把兜底也一起弄丢。
ICO_API ico_status image_codec_offload_set_order(ico_codec *codec,
                                                 const int *kinds, int count);

// 探测每个已注册后端的可用性与原因。out 由调用方分配，capacity 为条目数。
ICO_API ico_status image_codec_offload_probe_backends(ico_codec *codec,
                                                      ico_backend_info *out,
                                                      int capacity, int *count);

// 最近一次调用的实际后端与所有尝试记录（含回退原因）。
// 这是本库最重要的一条诊断信息：不允许只看到「用了 CPU」却说不出为什么。
ICO_API ico_status image_codec_offload_last_outcome(ico_codec *codec,
                                                    int *used_backend,
                                                    char *detail,
                                                    size_t detail_capacity);

// 最近一次成功/失败调用所选择的执行模式；与 last_outcome 的后端字段配对。
ICO_API ico_status image_codec_offload_last_execution_mode(
    ico_codec *codec, int *mode);

// 这个句柄当前能表达的编码参数（见 ico_encode_capabilities）。
// 自动模式下是**各后端的并集**：某个后端不支持时路由层会换下一个，
// 只要顺序里有一个能做，请求就能成立。强制后端时就是该后端自己的表。
ICO_API ico_status image_codec_offload_encode_capabilities(
    ico_codec *codec, ico_encode_capabilities *out);

// 按魔数探测容器格式（不看扩展名）。无法识别时 *out_format = UNKNOWN 且返回 OK。
ICO_API ico_status image_codec_offload_probe(const void *data, size_t size,
                                             int *out_format);

// 解码内存 buffer -> ico_image。
// format 传 ICO_FORMAT_UNKNOWN(-1) 时按魔数自动探测。
ICO_API ico_status image_codec_offload_decode(ico_codec *codec, int format,
                                              const void *in, size_t in_len,
                                              ico_image *out);

// 编码 ico_image -> 内存 buffer。
// out/out_capacity 为输出缓冲；*out_len 返回实际（成功）或所需（不足）字节数，
// 缓冲不足返回 ICO_ERR_OUT_OF_MEMORY，支持「先查后写」。
ICO_API ico_status image_codec_offload_encode(ico_codec *codec, int format,
                                              const ico_image *img,
                                              const ico_encode_params *params,
                                              void *out, size_t *out_len,
                                              size_t out_capacity);

// Extended encoding entry point. Kept separate from ico_encode_params so the
// existing C struct layout and image_codec_offload_encode ABI remain intact.
// jpeg2000_container is ico_jpeg2000_container; other values are rejected.
ICO_API ico_status image_codec_offload_encode_ex(
    ico_codec *codec, int format, const ico_image *img,
    const ico_encode_params *params, int jpeg2000_container, void *out,
    size_t *out_len, size_t out_capacity);

// 真正有界内存的 JPEG 2000 行流式编码。该入口当前只使用 OpenJPEG CPU tile
// writer，不会把所有行聚合为 ico_image，也不会声明 GPU 参与。output_path 是
// UTF-8 文件路径；调用方负责先写临时路径、成功后再原子替换最终输出。
ICO_API ico_status image_codec_offload_encode_jpeg2000_rows(
    ico_codec *codec, const char *output_path, int width, int height,
    int channels, int container, ico_row_reader row_reader, void *user);

// JPEG XL RGB24 分块行输入编码。libjxl 可能以任意顺序、多次请求同一行的
// 子区间，因此 row_reader 必须支持随机读取；该接口使用 CPU/libjxl，不表示 GPU。
ICO_API ico_status image_codec_offload_encode_jpegxl_rows(
    ico_codec *codec, const char *output_path, int width, int height,
    int channels, ico_row_reader row_reader, void *user);

// ---- TIFF 分片（把一帧按 strip 切给多条 lane 并行压缩）----
//
// 用法：每条 lane 调一次 image_codec_offload_tiff_strip_encode 压自己那段行，
// 全部完成后调一次 image_codec_offload_tiff_assemble 拼成文件。
//
// 正确性契约：分片结果与「同一张图、同一组参数一次性编码」**逐字节相同**。
// 这要求两件事同时成立：① 每片的压缩流与整图多 strip 编码的对应片逐位相同
// （各后端自己保证，靠 tiff_strip_sharding 能力位声明）；② 容器按 libtiff
// 的目录布局拼装（本库自己写，见 tiff_container.cpp）。
//
// 支持范围：compression = 1/5/8/32773（无压缩 / LZW / Deflate / PackBits），
// 且必须是 strip 布局（tiff_tile_size 必须为 0）。JPEG-in-TIFF 与 tiled 布局
// 返回 ICO_ERR_UNSUPPORTED_FORMAT，由调用方退回整图单 lane。

// 拼装所需的容器信息。压缩 / Predictor / DPI 从 params 取（与整图编码同一套
// 语义），因此这里不需要再传一遍。
typedef struct ico_tiff_assembly {
  int width;
  int height;
  int channels;       // 1 / 2 / 3 / 4
  int rows_per_strip; // > 0；条数必须等于 ceil(height / rows_per_strip)
  ico_encode_params params;
} ico_tiff_assembly;

// 把 ico_image 的 [first_row, first_row + rows) 独立压缩成一条 strip 的压缩流
// （不含容器；由 image_codec_offload_tiff_assemble 负责拼）。
// out/out_len/out_capacity 语义与 image_codec_offload_encode 一致。
ICO_API ico_status image_codec_offload_tiff_strip_encode(
    ico_codec *codec, const ico_image *img, int first_row, int rows,
    const ico_encode_params *params, void *out, size_t *out_len,
    size_t out_capacity);

// 把已经压好的 strips 按行顺序拼成一份 TIFF。
// strips[i] / strip_sizes[i] 是第 i 条的压缩流（可为 NULL，仅当对应长度为 0）。
// 不需要句柄：拼装是纯容器操作，与后端无关。
ICO_API ico_status image_codec_offload_tiff_assemble(
    const ico_tiff_assembly *assembly, const void *const *strips,
    const size_t *strip_sizes, int strip_count, void *out, size_t *out_len,
    size_t out_capacity);

// 释放 ico_image（含 data）。NULL 安全。
ICO_API void image_codec_offload_image_free(ico_image *img);

// ---- 画质指标 ----
// 三项各自独立，只算需要的那个（代价差得很远）。两张图必须同尺寸、同通道，
// 否则返回 ICO_ERR_INVALID_ARGUMENT（不做隐式缩放）。
//
// 定义在三个后端之间完全一致，换后端不会换数字：
//   MSE     = Σ(x-y)² / N
//   PSNR    = 10·log10(255²/MSE)；MSE == 0 时固定 99.0（有限值，不是 Inf）
//   SSIM    = 11x11 高斯窗(σ=1.5) 的均值，复制边缘，K1=0.01/K2=0.03/L=255
//   MS-SSIM = 5 级金字塔(2x2 均值下采样)，
//             Π CS_j^w_j · SSIM_last^w_last，
//             w = 0.0448/0.2856/0.3001/0.2363/0.1333（小图按实际尺度数归一化）
ICO_API ico_status image_codec_offload_metric_mse(ico_codec *codec,
                                                  const ico_image *a,
                                                  const ico_image *b,
                                                  double *out_value);
ICO_API ico_status image_codec_offload_metric_psnr(ico_codec *codec,
                                                   const ico_image *a,
                                                   const ico_image *b,
                                                   double *out_value);
ICO_API ico_status image_codec_offload_metric_ssim(ico_codec *codec,
                                                   const ico_image *a,
                                                   const ico_image *b,
                                                   ico_ssim *out);

#ifdef __cplusplus
} // extern "C"
#endif
