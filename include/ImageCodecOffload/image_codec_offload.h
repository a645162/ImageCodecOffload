// image_codec_offload.h - ImageCodecOffload 对外 C API（纯 C，C11）。
//
// 这一层是给**外部程序**（ImageViewer、批处理工具、C# 互操作）用的：它只面向
// 「格式 + 像素 + 参数」，不暴露任何后端类型。调用方不需要知道本机有 OpenCL
// 还是有 CUDA，也不需要知道某个格式在当前后端能不能用。
//
// 用法：
//   #include <ImageCodecOffload/image_codec_offload.h>
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
// 数值与三个后端子项目（ImageCodecOpenCL / ImageCodecCUDA / ImageCodecVulkan）
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

// 编码参数。字段按目标格式选用，未用到的字段忽略（保持 0 即用默认值）。
typedef struct ico_encode_params {
  // JPEG
  int jpeg_quality;     // 1..100，0 = 默认 90
  int jpeg_subsampling; // 0 = 默认(420)，444/422/420

  // TIFF
  int tiff_compression;    // 0 = 默认 LZW，1 = none，5 = LZW，8 = deflate
  int tiff_rows_per_strip; // 0 = 自动
  int tiff_tile_size;      // 0 = strip 布局；>0 = tiled（16 的倍数）

  // PNG
  int png_compression; // 0..9，-1 = 默认 6
  int png_filter;      // -1 = 自适应，0..4 = 固定滤波器
} ico_encode_params;

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
