// File      : image_codec_vulkan.h
// Brief     : Vulkan 编解码后端对外 C API（纯 C，C11）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 用法：
//   #include <Image/Codec/Vulkan/image_codec_vulkan.h>
//   链接 Image.Codec.Vulkan::CodecApi（或聚合目标）。
//
// 设计约束（与 Image.Codec.CUDA / Image.Codec.OpenCL 一一对应，便于路由层只写一套）：
//   * 全部入口都是内存 buffer 语义，不落地临时文件；
//   * 所有函数返回 image_codec_vulkan_status，0 表示成功；状态码数值与另外两个
//     后端完全相同，上层可以把任何后端的失败码直接映射过来；
//   * 通道数就是真实样本数（1/2/3/4），绝不静默升维或丢 Alpha；做不到的组合
//     返回 UNSUPPORTED_FORMAT 让上层回退，而不是给一个「看着成功」的结果；
//   * 线程安全：本 API 内部不加锁，同一 codec 句柄并发调用需调用方同步。
//
// 与 Image.Codec.Vulkan.Api 的区别：那个库是 Windows 专用的 GL 互操作
// （WGL + VK_KHR_external_memory_win32），本库是平台无关的编解码 C API。
// 两者名字接近但职责无关，不要合并。
#pragma once

#include <stddef.h>
#include <stdint.h>

// ---- 导出宏 ----
// 静态库构建：IMAGE_CODEC_VULKAN_API 为空。共享库构建时由构建方定义
// IMAGE_CODEC_VULKAN_BUILD_SHARED + IMAGE_CODEC_VULKAN_BUILDING_LIB。
#if defined(_WIN32) && defined(IMAGE_CODEC_VULKAN_BUILD_SHARED)
#if defined(IMAGE_CODEC_VULKAN_BUILDING_LIB)
#define IMAGE_CODEC_VULKAN_API __declspec(dllexport)
#else
#define IMAGE_CODEC_VULKAN_API __declspec(dllimport)
#endif
#else
#define IMAGE_CODEC_VULKAN_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- 状态码 ----
// 数值与 Image.Codec.CUDA / Image.Codec.OpenCL 对齐，上层可以只用一套 switch。
typedef enum image_codec_vulkan_status {
  IMAGE_CODEC_VULKAN_OK = 0, // 成功
  IMAGE_CODEC_VULKAN_ERR_INVALID_ARGUMENT =
      1, // 参数非法（空指针/越界/不支持的参数组合）
  IMAGE_CODEC_VULKAN_ERR_OUT_OF_MEMORY = 2, // 内存不足（含输出缓冲过小）
  IMAGE_CODEC_VULKAN_ERR_BACKEND_UNAVAILABLE =
      3, // 没有可用 Vulkan 设备 / 驱动不支持 fp64
  IMAGE_CODEC_VULKAN_ERR_UNSUPPORTED_FORMAT = 4, // 不支持的格式/子格式组合
  IMAGE_CODEC_VULKAN_ERR_DECODE_FAILED = 5,      // 解码失败
  IMAGE_CODEC_VULKAN_ERR_ENCODE_FAILED = 6,      // 编码失败
  IMAGE_CODEC_VULKAN_ERR_IO = 7,                 // 文件/临时文件 I/O 错误
  IMAGE_CODEC_VULKAN_ERR_INTERNAL = 8            // 内部错误
} image_codec_vulkan_status;

// 容器格式。数值与 Image.Codec.CUDA 的 0/1/2 兼容。
typedef enum image_codec_vulkan_format {
  IMAGE_CODEC_VULKAN_FORMAT_JPEG = 0,
  IMAGE_CODEC_VULKAN_FORMAT_TIFF = 1,
  IMAGE_CODEC_VULKAN_FORMAT_PNG = 2,
  IMAGE_CODEC_VULKAN_FORMAT_UNKNOWN = -1
} image_codec_vulkan_format;

// 解码产物 / 编码输入。
//   channels == 1：灰度；2：灰度+Alpha；3：RGB；4：RGBA。
//   data 为 top-down 行序（第 0 行为图像顶部）；stride == width * channels。
//   data 由本库 malloc，调用方用 image_codec_vulkan_image_free 释放。
typedef struct image_codec_vulkan_image {
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint8_t *data;
  size_t stride;    // 每行字节数（= width * channels）
  size_t data_size; // 像素数据总字节数
} image_codec_vulkan_image;

// 编码参数。字段按目标格式选用，未用到的字段忽略：
//   JPEG: jpeg_quality（1..100）、jpeg_subsampling（0=自动，444/420/422）
//   TIFF: tiff_compression（1=none, 5=LZW）、tiff_rows_per_strip（0=自动）、
//         tiff_tile_size（>0 改为 tiled 布局，必须是 16 的倍数）、
//         tiff_predictor（1=关闭，2=水平差分；0 取默认 2）
//   PNG:  png_compression（0..9）、png_filter（-1=自适应，0..4=固定）
//
// band_rows 是 GPU 每批行数的调参旋钮：显存占用只与它和行宽有关，与图像高度
// 无关。默认 0 = 库内默认（256）。不同 GPU 上最优值不同，先用内部 C++ 入口
// 扫描出结果，再把它固定到这里。
typedef struct image_codec_vulkan_encode_params {
  int jpeg_quality;
  int jpeg_subsampling;
  int tiff_compression;
  int tiff_rows_per_strip;
  int tiff_tile_size;
  int tiff_predictor;
  int png_compression;
  int png_filter;
  int band_rows;
} image_codec_vulkan_encode_params;

// SSIM 与 MS-SSIM 的结果。两者共享第 0 级的 11x11 窗统计（MS-SSIM 的第 0 级
// 就是 SSIM），所以由一次调用一起给出——拆成两个函数只会把最贵的那一级算两遍。
typedef struct image_codec_vulkan_ssim {
  double ssim;
  double ms_ssim;
  int levels; // MS-SSIM 实际使用的尺度数（1..5；小图会少于 5）
} image_codec_vulkan_ssim;

// 显存信息（字节）。用于上层在「要不要把这张大图交给 Vulkan 后端」时做决定。
typedef struct image_codec_vulkan_memory_info {
  size_t total_bytes;     // 设备总显存
  size_t free_bytes;      // 设备当前空闲（0 = 驱动不提供 VK_EXT_memory_budget）
  size_t max_alloc_bytes; // 单次分配上限（最大的 device-local 堆）
  size_t budget_bytes;    // 本句柄的显存上限（0 = 不限制）
  size_t used_bytes;      // 本句柄当前占用
  size_t peak_bytes;      // 本句柄历史峰值
} image_codec_vulkan_memory_info;

// 后端能力表。上层 router 据此决定「哪些格式真的能交给本后端」。
// 某项为 0 时，该格式/方向必须回退到其它后端，而不是让调用方自己猜。
typedef struct image_codec_vulkan_capabilities {
  char device_name[256];
  uint32_t api_version;
  size_t total_memory;
  int jpeg_decode;        // 1 = 支持
  int jpeg_encode;        // 1 = 支持
  int tiff_decode;        // 1 = 支持（容器 + LZW + 预测器）
  int tiff_encode;        // 1 = 支持
  int png_decode;         // 1 = 支持（inflate/unfilter 在 CPU）
  int png_encode;         // 1 = 支持
  int tiff_predictor_gpu; // 1 = TIFF Predictor=2 在 GPU 上完成
  int png_filter_gpu;     // 1 = PNG 行滤波在 GPU 上完成
  int tiff_lzw_gpu;       // 1 = LZW 编码在 GPU 上完成（本后端为 0，见 note）
  char note[256];         // 路由/降级原因（可为空）
} image_codec_vulkan_capabilities;

// 不透明编解码器句柄。
typedef struct image_codec_vulkan_codec image_codec_vulkan_codec;

// 状态码 -> 可读字符串（静态存储，勿 free）。
IMAGE_CODEC_VULKAN_API const char *
image_codec_vulkan_status_string(image_codec_vulkan_status status);

// 创建编解码器（绑定 device_index 指定的 Vulkan 设备，-1 = 自动选独显优先）。
// 设备不可用（没有 Vulkan 设备 / 没有 compute 队列）时返回
// IMAGE_CODEC_VULKAN_ERR_BACKEND_UNAVAILABLE。
//
// 没有 shaderFloat64 的设备**仍然可以创建**：编解码（JPEG/PNG/TIFF）只用 fp32，
// 只有指标必须 double 累加，届时指标调用自己返回 BackendUnavailable 并附原因，
// 由上层回退 CPU 指标。把整块卡因为一个可选算子判死刑，会让只有核显的机器
// 完全用不上本后端。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status
image_codec_vulkan_codec_create(image_codec_vulkan_codec **out_codec,
                                int device_index);

// 销毁编解码器。NULL 安全。
IMAGE_CODEC_VULKAN_API void
image_codec_vulkan_codec_destroy(image_codec_vulkan_codec *codec);

// 查询本句柄所在设备的实际能力（含降级说明）。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status
image_codec_vulkan_get_capabilities(image_codec_vulkan_codec *codec,
                                    image_codec_vulkan_capabilities *out);

// 是否允许使用 GPU。关闭后 PNG/TIFF 走 CPU 参考路径（产物与 GPU 逐字节相同），
// 指标准确性不受影响（指标本身必须 GPU 或 CPU 参考二选一，见 note）。
// 默认开启；用于「设备太慢/太小，宁可用 CPU」的部署。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status
image_codec_vulkan_set_use_gpu(image_codec_vulkan_codec *codec, int enable);

// ---- 显存 ----

// 不需要 codec 句柄即可查询某设备的显存（budget/used/peak 填 0）。
// 用于上层在创建句柄之前判断「这台机器能不能吃下这张图」。
// device_index = -1 表示按本后端规则自动选（独显优先）。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status
image_codec_vulkan_device_memory(int device_index,
                                 image_codec_vulkan_memory_info *out);

// 查询本句柄的显存：total/free/max_alloc 来自驱动，budget 是本句柄的上限。
//
// 注意 used/peak 恒为 0：Vulkan 后端每次操作内部临时建设备（编码/解码/指标
// 各自持有自己的缓冲生命周期），句柄本身不持有显存，因此这里如实报 0 而不是
// 编一个不存在的数字。想观察一次操作的真实占用，用 set_memory_limit 卡上限
// 并读操作返回的状态（超限即 OUT_OF_MEMORY，且 C++ 入口的 err 会写明需要多少）。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status
image_codec_vulkan_get_memory_info(image_codec_vulkan_codec *codec,
                                   image_codec_vulkan_memory_info *out);

// 设置本句柄允许使用的显存上限（字节）。0 = 不限制（仍受设备空闲显存约束）。
// 超过上限的分配立即失败并返回 ERR_OUT_OF_MEMORY —— 大图在小显存机器上是
// 「明确失败」，而不是把整机拖进 swap 或触发驱动重置。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status
image_codec_vulkan_set_memory_limit(image_codec_vulkan_codec *codec,
                                    size_t limit_bytes);

// 按魔数探测容器格式（不看扩展名）。无法识别时 *out_format = UNKNOWN 且返回 OK。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status
image_codec_vulkan_probe(const void *data, size_t size, int *out_format);

// 解码内存 buffer -> image_codec_vulkan_image。
// format 传 IMAGE_CODEC_VULKAN_FORMAT_UNKNOWN(-1) 时自动探测。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status image_codec_vulkan_decode(
    image_codec_vulkan_codec *codec, int format, const void *in, size_t in_len,
    image_codec_vulkan_image *out);

// 编码 image_codec_vulkan_image -> 内存 buffer。
// out/out_capacity 为输出缓冲；*out_len 返回实际（成功）或所需（缓冲不足）字节数。
// 缓冲不足返回 ERR_OUT_OF_MEMORY 并写回所需大小，支持「先查后写」。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status
image_codec_vulkan_encode(image_codec_vulkan_codec *codec, int format,
                          const image_codec_vulkan_image *img,
                          const image_codec_vulkan_encode_params *params,
                          void *out, size_t *out_len, size_t out_capacity);

// 将像素放入 Vulkan host-visible 设备缓冲再读回宿主内存。该入口不改变样本值，
// 供上层 Hybrid CPU/GPU 编解码路径验证设备 staging；码流编解码仍由上层 CPU 库完成。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status image_codec_vulkan_gpu_stage(
    image_codec_vulkan_codec *codec, const image_codec_vulkan_image *in,
    image_codec_vulkan_image *out);

// ---- 画质指标 ----
//
// 三项各自独立，只算需要的那个（代价差得很远：MSE 每像素读 2 字节，SSIM 要读
// 11x11 窗，MS-SSIM 还要再叠 5 级金字塔）。GPU 并行，大图按行分块流式处理，
// 显存占用与图像高度无关。
//
// 定义与其它后端一致，换后端不会换数字：
//   MSE     = Σ(x-y)² / N
//   PSNR    = 10·log10(255²/MSE)；MSE == 0 时固定为 99.0（有限值，不是 Inf）
//   SSIM    = 11x11 高斯窗(σ=1.5) 的均值，复制边缘，K1=0.01/K2=0.03/L=255
//   MS-SSIM = 5 级金字塔(2x2 均值下采样)，
//             Π CS_j^w_j · SSIM_last^w_last，
//             w = 0.0448/0.2856/0.3001/0.2363/0.1333（小图按实际尺度数归一化）
//
// 两张图必须同尺寸、同通道；不一致直接返回 ERR_INVALID_ARGUMENT（不做隐式缩放）。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status image_codec_vulkan_metric_mse(
    image_codec_vulkan_codec *codec, const image_codec_vulkan_image *a,
    const image_codec_vulkan_image *b, double *out_value);

IMAGE_CODEC_VULKAN_API image_codec_vulkan_status image_codec_vulkan_metric_psnr(
    image_codec_vulkan_codec *codec, const image_codec_vulkan_image *a,
    const image_codec_vulkan_image *b, double *out_value);

IMAGE_CODEC_VULKAN_API image_codec_vulkan_status image_codec_vulkan_metric_ssim(
    image_codec_vulkan_codec *codec, const image_codec_vulkan_image *a,
    const image_codec_vulkan_image *b, image_codec_vulkan_ssim *out);

// 释放 image_codec_vulkan_image（含 data）。NULL 安全。
IMAGE_CODEC_VULKAN_API void
image_codec_vulkan_image_free(image_codec_vulkan_image *img);

// 枚举本后端可用的 Vulkan 设备。名称为 "index: name (api X.Y, N MiB)" 形式。
// 只列出**真的能用于编解码**的设备（有 compute 队列且支持 fp64），因为指标
// 的 double 累加是硬性要求——列出一块用不了的卡只会让上层选到它再失败。
// names 由调用方用 image_codec_vulkan_device_list_free 释放；无设备时 count=0
// 且返回 OK。
IMAGE_CODEC_VULKAN_API image_codec_vulkan_status
image_codec_vulkan_device_list(char ***names, int *count);
IMAGE_CODEC_VULKAN_API void image_codec_vulkan_device_list_free(char **names,
                                                                int count);

#ifdef __cplusplus
} // extern "C"
#endif
