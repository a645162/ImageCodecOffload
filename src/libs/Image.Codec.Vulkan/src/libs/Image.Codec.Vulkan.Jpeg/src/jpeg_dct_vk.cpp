// File      : jpeg_dct_vk.cpp
// Brief     : JPEG 的 GPU DCT/颜色变换（Vulkan compute）实现
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#include "jpeg_dct_vk.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include "vk_buffer.h"
#include "vk_codec_device.h"
#include "vk_pipeline.h"

// 由 CMake 从 kernels/*.comp 生成（构建产物，不提交进仓库）。
#include "jpeg_spv_data.h"

namespace image_codec {
namespace vulkan {
namespace jpeg {

namespace {

// 与各着色器的 layout(local_size_x = 256) 一致。
constexpr size_t kLocalSize = 256;

size_t alignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

uint32_t groupCount(size_t items) {
  return static_cast<uint32_t>((items + kLocalSize - 1) / kLocalSize);
}

// ---- 与着色器 push constant 一一对应的结构体 ----
// 字段顺序必须和 .comp 里的 Params 完全相同：push constant 是裸字节拷贝，
// 顺序错一位不会编译失败，只会算出另一张图。

struct IdctParams {
  int32_t coeff_off;
  int32_t plane_off;
  int32_t plane_stride;
  int32_t block_stride;
  int32_t blocks_total;
  int32_t qtable_index;
};

struct YcbcrParams {
  int32_t max_h;
  int32_t max_v;
  int32_t ncomp;
  int32_t out_stride;
  int32_t width;
  int32_t height;
  int32_t out_channels;
};

struct PlaneParams { // gray_to_y / rgb_to_ycbcr
  int32_t src_stride;
  int32_t plane_stride;
  int32_t width;
  int32_t height;
  int32_t plane_w;
  int32_t plane_h;
};

struct DownsampleParams {
  int32_t src_stride;
  int32_t dst_stride;
  int32_t src_w;
  int32_t src_h;
  int32_t dst_w;
  int32_t dst_h;
  int32_t h_ratio; // 4:4:4 = 1，4:2:0 / 4:2:2 = 2
  int32_t v_ratio; // 4:4:4 / 4:2:2 = 1，4:2:0 = 2
};

struct FdctParams {
  int32_t plane_stride;
  int32_t qtable_index;
  int32_t coeff_off;
  int32_t block_stride;
  int32_t blocks_total;
};

// 把 4 张量化表（自然序）摊平成 float[256]，与 OpenCL 版 qtable 布局相同。
void buildQuantBuffer(const JpegImage &img, float out[4 * 64]) {
  for (int t = 0; t < 4; ++t) {
    for (int i = 0; i < 64; ++i) {
      out[t * 64 + i] =
          img.quant[t].present ? static_cast<float>(img.quant[t].q[i]) : 0.0f;
    }
  }
}

std::string bufferError(const char *what, const Buffer &buffer) {
  return std::string("JPEG: ") + what + " allocation failed (" +
         buffer.error() + ")";
}

} // namespace

void buildDctMatrix(float m[64][64]) {
  constexpr float kPi = 3.14159265358979323846f;
  for (int p = 0; p < 64; ++p) {
    const int u = p & 7;
    const int v = p >> 3;
    const float cu = (u == 0) ? 0.70710678f : 1.0f;
    const float cv = (v == 0) ? 0.70710678f : 1.0f;
    for (int j = 0; j < 64; ++j) {
      const int x = j & 7;
      const int y = j >> 3;
      m[p][j] = 0.25f * cu * cv * std::cos(kPi * (2 * x + 1) * u / 16.0f) *
                std::cos(kPi * (2 * y + 1) * v / 16.0f);
    }
  }
}

bool gpuDecode(const JpegImage &img, const std::vector<float> &coeffs,
               const DctOptions &options, std::vector<uint8_t> *out,
               std::string *err) {
  if (out == nullptr) {
    if (err != nullptr) *err = "JPEG: null output";
    return false;
  }
  out->clear();
  if (!img.valid) {
    if (err != nullptr) *err = "JPEG: invalid image description";
    return false;
  }

  Device device;
  std::string local;
  if (!device.init(options.device_index, &local)) {
    if (err != nullptr) *err = "JPEG: " + local;
    return false;
  }
  device.setMemoryLimit(options.memory_limit);

  // 分量平面：每分量按块网格补齐（blocks_x*8 x blocks_y*8），与 host 参考共用布局。
  size_t plane_floats = 0;
  for (int c = 0; c < img.ncomp; ++c) {
    plane_floats += static_cast<size_t>(img.comp[c].blocks_x * 8) *
                    static_cast<size_t>(img.comp[c].blocks_y * 8);
  }

  float quant[4 * 64] = {0.0f};
  buildQuantBuffer(img, quant);
  float matrix[64][64];
  buildDctMatrix(matrix);

  const size_t coeff_bytes =
      std::max<size_t>(coeffs.size() * sizeof(float), 16);
  Buffer coeff_buf(&device, coeff_bytes);
  Buffer quant_buf(&device, sizeof(quant));
  Buffer matrix_buf(&device, sizeof(matrix));
  Buffer planes_buf(&device,
                    std::max<size_t>(plane_floats * sizeof(float), 16));
  Buffer desc_buf(&device, 3 * 6 * sizeof(int32_t));
  if (!coeff_buf.ok()) {
    if (err != nullptr) *err = bufferError("coeffs", coeff_buf);
    return false;
  }
  if (!quant_buf.ok()) {
    if (err != nullptr) *err = bufferError("quant", quant_buf);
    return false;
  }
  if (!matrix_buf.ok()) {
    if (err != nullptr) *err = bufferError("dct matrix", matrix_buf);
    return false;
  }
  if (!planes_buf.ok()) {
    if (err != nullptr) *err = bufferError("planes", planes_buf);
    return false;
  }
  if (!desc_buf.ok()) {
    if (err != nullptr) *err = bufferError("desc", desc_buf);
    return false;
  }

  const int out_channels = (img.ncomp == 1) ? 1 : 3;
  const size_t out_bytes = static_cast<size_t>(img.width) *
                           static_cast<size_t>(img.height) *
                           static_cast<size_t>(out_channels);
  const size_t out_words = alignUp(out_bytes, 4) / 4;
  Buffer out_buf(&device, out_words * sizeof(uint32_t));
  if (!out_buf.ok()) {
    if (err != nullptr) *err = bufferError("output", out_buf);
    return false;
  }

  if (!coeff_buf.upload(coeffs.data(), coeffs.size() * sizeof(float)) ||
      !quant_buf.upload(quant, sizeof(quant)) ||
      !matrix_buf.upload(matrix, sizeof(matrix))) {
    if (err != nullptr) *err = "JPEG: upload failed";
    return false;
  }
  planes_buf.zero();

  // ---- 1. 反量化 + IDCT（每分量一次）----
  Pipeline idct;
  const std::vector<BufferBinding> idct_bindings = {
      {0, false}, // coeffs
      {1, false}, // qtable
      {2, false}, // dct matrix
      {3, true},  // planes
  };
  if (!idct.init(device, vic_jpeg_dequant_idct, vic_jpeg_dequant_idct_size,
                 idct_bindings, sizeof(IdctParams), &local)) {
    if (err != nullptr) *err = "JPEG: " + local;
    return false;
  }

  for (int c = 0; c < img.ncomp; ++c) {
    const JpegComponentInfo &ci = img.comp[c];
    const int blocks = ci.blocks_x * ci.blocks_y;
    if (blocks <= 0) continue;

    IdctParams params{};
    params.coeff_off = ci.coeff_off;
    params.plane_off = ci.plane_off;
    params.plane_stride = ci.blocks_x * 8;
    params.block_stride = ci.blocks_x;
    params.blocks_total = blocks;
    params.qtable_index = ci.tq;

    const std::vector<VkBuffer> buffers = {
        coeff_buf.vkBuffer(), quant_buf.vkBuffer(), matrix_buf.vkBuffer(),
        planes_buf.vkBuffer()};
    if (!idct.dispatch(buffers, &params, sizeof(params),
                       groupCount(static_cast<size_t>(blocks) * 64), &local)) {
      if (err != nullptr) *err = "JPEG: dequant+IDCT dispatch failed: " + local;
      return false;
    }
  }

  // ---- 2. YCbCr -> RGB + 上采样 ----
  int32_t desc[3 * 6] = {0};
  for (int c = 0; c < 3; ++c) {
    if (c < img.ncomp) {
      const JpegComponentInfo &ci = img.comp[c];
      desc[c * 6 + 0] = ci.plane_off;
      desc[c * 6 + 1] = ci.blocks_x * 8;
      desc[c * 6 + 2] = ci.plane_w;
      desc[c * 6 + 3] = ci.plane_h;
      desc[c * 6 + 4] = ci.h_samp;
      desc[c * 6 + 5] = ci.v_samp;
    } else {
      // 单分量图没有色度平面：给一个 1x1 的占位描述，着色器走
      // sample_c(0) 路径，不会读到这里；填 0 只是为了让缓冲有确定的初值。
      desc[c * 6 + 0] = 0;
      desc[c * 6 + 1] = 0;
      desc[c * 6 + 2] = 1;
      desc[c * 6 + 3] = 1;
      desc[c * 6 + 4] = img.max_h;
      desc[c * 6 + 5] = img.max_v;
    }
  }
  if (!desc_buf.upload(desc, sizeof(desc))) {
    if (err != nullptr) *err = "JPEG: desc upload failed";
    return false;
  }

  Pipeline color;
  const std::vector<BufferBinding> color_bindings = {
      {0, false}, // planes
      {1, false}, // desc
      {2, true},  // output
  };
  if (!color.init(device, vic_jpeg_ycbcr_to_rgb, vic_jpeg_ycbcr_to_rgb_size,
                  color_bindings, sizeof(YcbcrParams), &local)) {
    if (err != nullptr) *err = "JPEG: " + local;
    return false;
  }

  YcbcrParams params{};
  params.max_h = img.max_h;
  params.max_v = img.max_v;
  params.ncomp = img.ncomp;
  params.out_stride = img.width * out_channels;
  params.width = img.width;
  params.height = img.height;
  params.out_channels = out_channels;

  const std::vector<VkBuffer> color_buffers = {
      planes_buf.vkBuffer(), desc_buf.vkBuffer(), out_buf.vkBuffer()};
  if (!color.dispatch(color_buffers, &params, sizeof(params),
                      groupCount(out_words), &local)) {
    if (err != nullptr)
      *err = "JPEG: color conversion dispatch failed: " + local;
    return false;
  }

  out->assign(out_bytes, 0);
  if (!out_buf.download(out->data(), out_bytes)) {
    if (err != nullptr) *err = "JPEG: output download failed";
    out->clear();
    return false;
  }
  return true;
}

bool gpuEncode(const JpegImage &img, const uint8_t *pixels,
               const DctOptions &options, std::vector<float> *coeffs,
               std::string *err) {
  if (coeffs == nullptr || pixels == nullptr) {
    if (err != nullptr) *err = "JPEG: null input";
    return false;
  }
  coeffs->clear();
  if (!img.valid) {
    if (err != nullptr) *err = "JPEG: invalid image description";
    return false;
  }

  Device device;
  std::string local;
  if (!device.init(options.device_index, &local)) {
    if (err != nullptr) *err = "JPEG: " + local;
    return false;
  }
  device.setMemoryLimit(options.memory_limit);

  float quant[4 * 64] = {0.0f};
  buildQuantBuffer(img, quant);
  float matrix[64][64];
  buildDctMatrix(matrix);

  const int in_channels = (img.ncomp == 1) ? 1 : 3;
  const size_t src_bytes = static_cast<size_t>(img.width) *
                           static_cast<size_t>(img.height) *
                           static_cast<size_t>(in_channels);

  // 全分辨率平面（所有分量共用同一对齐尺寸）：色度是否下采样由后面的
  // downsample 决定，这样 4:4:4 与 4:2:0/4:2:2 共用一条编码路径。
  const int full_w = img.mcus_x * img.max_h * 8;
  const int full_h = img.mcus_y * img.max_v * 8;
  const size_t full_floats =
      static_cast<size_t>(full_w) * static_cast<size_t>(full_h);

  Buffer src_buf(&device, alignUp(src_bytes, 4));
  Buffer y_buf(&device, full_floats * sizeof(float));
  if (!src_buf.ok()) {
    if (err != nullptr) *err = bufferError("source pixels", src_buf);
    return false;
  }
  if (!y_buf.ok()) {
    if (err != nullptr) *err = bufferError("Y plane", y_buf);
    return false;
  }

  // 只有 3 分量才需要色度平面。Buffer 的默认构造不带设备（ensure() 只能在
  // 已有设备的实例上扩容），所以条件成立的缓冲必须显式绑定设备后再建。
  std::optional<Buffer> cb_buf;
  std::optional<Buffer> cr_buf;
  if (in_channels == 3) {
    cb_buf.emplace(&device, full_floats * sizeof(float));
    cr_buf.emplace(&device, full_floats * sizeof(float));
    if (!cb_buf->ok() || !cr_buf->ok()) {
      if (err != nullptr) *err = bufferError("chroma planes", *cb_buf);
      return false;
    }
  }

  if (!src_buf.upload(pixels, src_bytes)) {
    if (err != nullptr) *err = "JPEG: source upload failed";
    return false;
  }

  // ---- 1. 像素 -> 平面（灰度直通 / RGB->YCbCr）----
  PlaneParams plane{};
  plane.src_stride = img.width * in_channels;
  plane.plane_stride = full_w;
  plane.width = img.width;
  plane.height = img.height;
  plane.plane_w = full_w;
  plane.plane_h = full_h;

  if (in_channels == 1) {
    Pipeline gray;
    const std::vector<BufferBinding> bindings = {{0, false}, {1, true}};
    if (!gray.init(device, vic_jpeg_gray_to_y, vic_jpeg_gray_to_y_size,
                   bindings, sizeof(PlaneParams), &local)) {
      if (err != nullptr) *err = "JPEG: " + local;
      return false;
    }
    const std::vector<VkBuffer> buffers = {src_buf.vkBuffer(),
                                           y_buf.vkBuffer()};
    if (!gray.dispatch(buffers, &plane, sizeof(plane), groupCount(full_floats),
                       &local)) {
      if (err != nullptr) *err = "JPEG: gray->Y dispatch failed: " + local;
      return false;
    }
  } else {
    Pipeline rgb2y;
    const std::vector<BufferBinding> bindings = {
        {0, false}, {1, true}, {2, true}, {3, true}};
    if (!rgb2y.init(device, vic_jpeg_rgb_to_ycbcr, vic_jpeg_rgb_to_ycbcr_size,
                    bindings, sizeof(PlaneParams), &local)) {
      if (err != nullptr) *err = "JPEG: " + local;
      return false;
    }
    const std::vector<VkBuffer> buffers = {src_buf.vkBuffer(), y_buf.vkBuffer(),
                                           cb_buf->vkBuffer(),
                                           cr_buf->vkBuffer()};
    if (!rgb2y.dispatch(buffers, &plane, sizeof(plane), groupCount(full_floats),
                        &local)) {
      if (err != nullptr) *err = "JPEG: RGB->YCbCr dispatch failed: " + local;
      return false;
    }
  }

  // ---- 2. 色度下采样（非 4:4:4 时）----
  bool need_downsample = false;
  for (int c = 0; c < img.ncomp; ++c) {
    if (img.comp[c].h_samp != img.max_h || img.comp[c].v_samp != img.max_v) {
      need_downsample = true;
    }
  }

  std::optional<Buffer> ds_cb;
  std::optional<Buffer> ds_cr;
  if (need_downsample) {
    const JpegComponentInfo &ci = img.comp[1];
    const size_t ds_floats = static_cast<size_t>(ci.blocks_x * 8) *
                             static_cast<size_t>(ci.blocks_y * 8);
    ds_cb.emplace(&device, ds_floats * sizeof(float));
    ds_cr.emplace(&device, ds_floats * sizeof(float));
    if (!ds_cb->ok() || !ds_cr->ok()) {
      if (err != nullptr) *err = bufferError("downsampled chroma", *ds_cb);
      return false;
    }

    Pipeline downsample;
    const std::vector<BufferBinding> bindings = {{0, false}, {1, true}};
    if (!downsample.init(device, vic_jpeg_downsample, vic_jpeg_downsample_size,
                         bindings, sizeof(DownsampleParams), &local)) {
      if (err != nullptr) *err = "JPEG: " + local;
      return false;
    }

    DownsampleParams ds{};
    ds.src_stride = full_w;
    ds.src_w = full_w;
    ds.src_h = full_h;
    ds.dst_stride = ci.blocks_x * 8;
    ds.dst_w = ci.blocks_x * 8;
    ds.dst_h = ci.blocks_y * 8;
    // 抽取比例从采样因子推导，不能写死 2x2：4:2:2 只做水平抽取，色度平面
    // 与亮度等高（v_ratio = 1）。写死 2x2 会把下半幅色度摊平。
    ds.h_ratio = img.max_h / (ci.h_samp > 0 ? ci.h_samp : 1);
    ds.v_ratio = img.max_v / (ci.v_samp > 0 ? ci.v_samp : 1);
    if (ds.h_ratio < 1) ds.h_ratio = 1;
    if (ds.v_ratio < 1) ds.v_ratio = 1;

    const size_t ds_items =
        static_cast<size_t>(ds.dst_w) * static_cast<size_t>(ds.dst_h);
    const std::vector<VkBuffer> cb_buffers = {cb_buf->vkBuffer(),
                                              ds_cb->vkBuffer()};
    if (!downsample.dispatch(cb_buffers, &ds, sizeof(ds), groupCount(ds_items),
                             &local)) {
      if (err != nullptr)
        *err = "JPEG: Cb downsample dispatch failed: " + local;
      return false;
    }
    const std::vector<VkBuffer> cr_buffers = {cr_buf->vkBuffer(),
                                              ds_cr->vkBuffer()};
    if (!downsample.dispatch(cr_buffers, &ds, sizeof(ds), groupCount(ds_items),
                             &local)) {
      if (err != nullptr)
        *err = "JPEG: Cr downsample dispatch failed: " + local;
      return false;
    }
  }

  // ---- 3. FDCT + 量化（每分量一次）----
  Buffer quant_buf(&device, sizeof(quant));
  Buffer matrix_buf(&device, sizeof(matrix));
  const size_t coeff_floats = static_cast<size_t>(img.total_blocks) * 64;
  Buffer coeff_buf(&device, coeff_floats * sizeof(float));
  if (!quant_buf.ok()) {
    if (err != nullptr) *err = bufferError("quant", quant_buf);
    return false;
  }
  if (!matrix_buf.ok()) {
    if (err != nullptr) *err = bufferError("dct matrix", matrix_buf);
    return false;
  }
  if (!coeff_buf.ok()) {
    if (err != nullptr) *err = bufferError("coeffs", coeff_buf);
    return false;
  }
  if (!quant_buf.upload(quant, sizeof(quant)) ||
      !matrix_buf.upload(matrix, sizeof(matrix))) {
    if (err != nullptr) *err = "JPEG: upload failed";
    return false;
  }

  Pipeline fdct;
  const std::vector<BufferBinding> fdct_bindings = {
      {0, false}, // plane
      {1, false}, // qtable
      {2, false}, // dct matrix
      {3, true},  // coeffs
  };
  if (!fdct.init(device, vic_jpeg_fdct_quantize, vic_jpeg_fdct_quantize_size,
                 fdct_bindings, sizeof(FdctParams), &local)) {
    if (err != nullptr) *err = "JPEG: " + local;
    return false;
  }

  for (int c = 0; c < img.ncomp; ++c) {
    const JpegComponentInfo &ci = img.comp[c];
    const int blocks = ci.blocks_x * ci.blocks_y;
    if (blocks <= 0) continue;

    VkBuffer plane = y_buf.vkBuffer();
    int32_t plane_stride = full_w;
    if (c == 1) {
      plane = need_downsample ? ds_cb->vkBuffer() : cb_buf->vkBuffer();
      if (need_downsample) plane_stride = ci.blocks_x * 8;
    } else if (c == 2) {
      plane = need_downsample ? ds_cr->vkBuffer() : cr_buf->vkBuffer();
      if (need_downsample) plane_stride = ci.blocks_x * 8;
    }

    FdctParams params{};
    params.plane_stride = plane_stride;
    params.qtable_index = ci.tq;
    params.coeff_off = ci.coeff_off;
    params.block_stride = ci.blocks_x;
    params.blocks_total = blocks;

    const std::vector<VkBuffer> buffers = {plane, quant_buf.vkBuffer(),
                                           matrix_buf.vkBuffer(),
                                           coeff_buf.vkBuffer()};
    if (!fdct.dispatch(buffers, &params, sizeof(params),
                       groupCount(static_cast<size_t>(blocks) * 64), &local)) {
      if (err != nullptr) *err = "JPEG: FDCT dispatch failed: " + local;
      return false;
    }
  }

  coeffs->assign(coeff_floats, 0.0f);
  if (!coeff_buf.download(coeffs->data(), coeff_floats * sizeof(float))) {
    if (err != nullptr) *err = "JPEG: coefficient download failed";
    coeffs->clear();
    return false;
  }
  return true;
}

} // namespace jpeg
} // namespace vulkan
} // namespace image_codec
