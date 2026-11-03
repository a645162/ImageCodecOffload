/*
 * File: image_codec.h
 * Brief: Image codec.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// image_codec.h - OpenCL-Image-Codec 对外 C API（纯 C，C11）。
//
// 用法：
//   #include <Image/Codec/OpenCL/image_codec.h>
//   链接 Image.Codec.OpenCL::All（或静态库 Image.Codec.OpenCL.Api）。
//
// 线程安全：本 API 未做内部加锁；同一 image_codec_codec* 句柄并发调用需调用方同步。
// 所有函数返回 image_codec_status；0(IMAGE_CODEC_OK) 表示成功。
#pragma once

#include <stddef.h>
#include <stdint.h>

// ---- 导出宏 ----
// 静态库构建：IMAGE_CODEC_API 为空。共享库构建（IMAGE_CODEC_BUILD_SHARED）时，
// 构建库的编译单元需定义 IMAGE_CODEC_BUILDING_LIB（dllexport），消费方自动 dllimport。
#if defined(_WIN32) && defined(IMAGE_CODEC_BUILD_SHARED)
#if defined(IMAGE_CODEC_BUILDING_LIB)
#define IMAGE_CODEC_API __declspec(dllexport)
#else
#define IMAGE_CODEC_API __declspec(dllimport)
#endif
#else
#define IMAGE_CODEC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- 状态码 ----
typedef enum image_codec_status {
  IMAGE_CODEC_OK = 0, // 成功
  IMAGE_CODEC_ERR_INVALID_ARGUMENT =
      1,                             // 参数非法（空指针/越界/不支持的参数组合）
  IMAGE_CODEC_ERR_OUT_OF_MEMORY = 2, // 内存不足（含输出缓冲过小）
  IMAGE_CODEC_ERR_BACKEND_UNAVAILABLE = 3, // 指定后端不可用（如缺 libvpl.dll）
  IMAGE_CODEC_ERR_UNSUPPORTED_FORMAT = 4,  // 不支持的格式/组合
  IMAGE_CODEC_ERR_DECODE_FAILED = 5,       // 解码失败
  IMAGE_CODEC_ERR_ENCODE_FAILED = 6,       // 编码失败
  IMAGE_CODEC_ERR_IO = 7,                  // 文件/临时文件 I/O 错误
  IMAGE_CODEC_ERR_INTERNAL = 8             // 内部错误
} image_codec_status;

// 后端：优先选择顺序 opencl / cpu。
typedef enum image_codec_backend {
  IMAGE_CODEC_BACKEND_OPENCL = 0, // OpenCL（GPU）
  IMAGE_CODEC_BACKEND_CPU = 2     // 纯 CPU
} image_codec_backend;

// 图像容器格式。数值与其它后端（CUDA / Vulkan）保持一致。
typedef enum image_codec_format {
  IMAGE_CODEC_FORMAT_JPEG = 0,
  IMAGE_CODEC_FORMAT_TIFF = 1,
  IMAGE_CODEC_FORMAT_PNG = 2
} image_codec_format;

// 解码产物/编码输入：top-down 行序（第 0 行为图像顶部）。
// channels：1 = 灰度、2 = 灰度+Alpha、3 = RGB、4 = RGBA，Alpha 一律是**非预乘**
// 的原始样本（本库不做合成）。
// **通道保真**：1 通道源解出来仍是 1 通道，4 通道源的 Alpha 也不会被悄悄丢掉。
// 各格式的实际能力：PNG / TIFF 支持 1..4；JPEG 没有 Alpha 样本，只接受 1 或 3，
// 2/4 返回 IMAGE_CODEC_ERR_UNSUPPORTED_FORMAT（不会降维冒充成功）。
// data 指向 width*height*channels 字节；stride 为每行字节数（通常 = width*channels）。
typedef struct image_codec_image {
  uint32_t width;
  uint32_t height;
  uint32_t channels; // 1 = 灰度, 2 = 灰度+A, 3 = RGB, 4 = RGBA
  uint8_t *data;     // malloc'd，由 image_codec_image_free 释放
  size_t stride;     // 每行字节数
  size_t data_size;  // 像素数据字节数
} image_codec_image;

// 编码参数。字段按目标格式选用：
//   JPEG: jpeg_quality（1..100）
//   TIFF: tiff_compression（1=none, 5=LZW）、tiff_rows_per_strip（0=自动/整幅）
//   PNG:  png_compression（0..9，<0 用默认 6）、png_filter（-1=自适应，0..4=固定）
typedef struct image_codec_encode_params {
  int jpeg_quality;
  // 0 = 按质量自动（>=92 用 4:4:4，否则 4:2:0）；444 / 422 / 420 = 强制。
  // 灰度（1 通道）恒为 4:4:4：没有色度可下采样。
  int jpeg_subsampling;
  int tiff_compression;
  int tiff_rows_per_strip;
  int png_compression;
  int png_filter;
} image_codec_encode_params;

// 不透明编解码器句柄。
typedef struct image_codec_codec image_codec_codec;

// 状态码 -> 可读字符串（静态存储，勿 free）。
IMAGE_CODEC_API const char *
image_codec_status_string(image_codec_status status);

// 创建编解码器（绑定后端）。成功返回 IMAGE_CODEC_OK 并写出句柄；失败返回错误码。
IMAGE_CODEC_API image_codec_status image_codec_codec_create(
    image_codec_codec **out_codec, image_codec_backend backend);

// 销毁编解码器。NULL 安全。
IMAGE_CODEC_API void image_codec_codec_destroy(image_codec_codec *codec);

// 解码内存 buffer -> image_codec_image（malloc'd，调用方用 image_codec_image_free 释放）。
// in/in_len 为编码后数据（JPEG 或 TIFF 字节流）。
IMAGE_CODEC_API image_codec_status image_codec_codec_decode(
    image_codec_codec *codec, image_codec_format format, const uint8_t *in,
    size_t in_len, image_codec_image *out);

// 编码 image_codec_image -> 内存 buffer。
// out/out_capacity 为输出缓冲；*out_len 返回实际/所需字节数。
// 若缓冲过小返回 IMAGE_CODEC_ERR_OUT_OF_MEMORY，*out_len 置为所需大小（支持先查后写）。
IMAGE_CODEC_API image_codec_status image_codec_codec_encode(
    image_codec_codec *codec, image_codec_format format,
    const image_codec_image *img, const image_codec_encode_params *params,
    uint8_t *out, size_t *out_len, size_t out_capacity);

// 将像素分块送入 OpenCL 再读回宿主内存。该入口不改变样本值，供上层
// Hybrid CPU/GPU 编解码路径验证 GPU staging；码流编解码仍由上层 CPU 库完成。
IMAGE_CODEC_API image_codec_status image_codec_codec_gpu_stage(
    image_codec_codec *codec, const image_codec_image *in,
    image_codec_image *out);

// 释放 image_codec_image（含 data）。NULL 安全。
IMAGE_CODEC_API void image_codec_image_free(image_codec_image *img);

// ---- 画质指标（OpenCL 并行；两张图必须同尺寸、同通道）----
//
// 三个入口各自独立，只算需要的那个：MSE 每像素读 2 字节，SSIM 要读 11x11 窗，
// MS-SSIM 还要再叠 5 级金字塔。SSIM 与 MS-SSIM 合并成一个入口，是因为
// MS-SSIM 的第 0 级就是 SSIM 的那一次统计——拆开只会把最贵的一级算两遍。
//
// 定义与其它后端（CUDA / Vulkan）完全一致，换后端不会换数字：
//   MSE     = Σ(x-y)² / N
//   PSNR    = 10·log10(255²/MSE)；MSE == 0 时固定 99.0（有限值，不是 Inf）
//   SSIM    = 11x11 高斯窗(σ=1.5) 的均值，复制边缘，K1=0.01/K2=0.03/L=255
//   MS-SSIM = 5 级金字塔(2x2 均值下采样)，
//             Π CS_j^w_j · SSIM_last^w_last，
//             w = 0.0448/0.2856/0.3001/0.2363/0.1333（小图按实际尺度数归一化）
//
// 尺寸或通道不一致直接返回 IMAGE_CODEC_ERR_INVALID_ARGUMENT（不做隐式缩放）。
typedef struct image_codec_ssim {
  double ssim;
  double ms_ssim; // 与 ssim 同源，不额外读一遍原图
  int levels;     // MS-SSIM 实际使用的尺度数（1..5）
} image_codec_ssim;

IMAGE_CODEC_API image_codec_status
image_codec_metric_mse(image_codec_codec *codec, const image_codec_image *a,
                       const image_codec_image *b, double *out_value);

IMAGE_CODEC_API image_codec_status
image_codec_metric_psnr(image_codec_codec *codec, const image_codec_image *a,
                        const image_codec_image *b, double *out_value);

IMAGE_CODEC_API image_codec_status
image_codec_metric_ssim(image_codec_codec *codec, const image_codec_image *a,
                        const image_codec_image *b, image_codec_ssim *out);

// 枚举 OpenCL 设备。成功返回 IMAGE_CODEC_OK；无设备时 count=0、names=NULL 仍返回 IMAGE_CODEC_OK。
// names 由调用方用 image_codec_device_list_free 释放。
IMAGE_CODEC_API image_codec_status image_codec_device_list(char ***names,
                                                           int *count);
IMAGE_CODEC_API void image_codec_device_list_free(char **names, int count);

#ifdef __cplusplus
} // extern "C"
#endif
