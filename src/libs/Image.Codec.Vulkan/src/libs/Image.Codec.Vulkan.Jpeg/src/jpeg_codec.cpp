// File      : jpeg_codec.cpp
// Brief     : baseline JPEG 编解码编排实现
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
#include "jpeg_codec.h"

#include <vector>

#include "jpeg_bitstream.h"
#include "jpeg_dct_cpu.h"
#include "jpeg_dct_vk.h"
#include "jpeg_huffman.h"

namespace image_codec {
namespace vulkan {
namespace jpeg {

namespace {

// 采样因子 -> 可读的子采样标签（444/420/422）。
void inferSubsampling(const JpegImage &img, int *out) {
  if (img.ncomp == 1) {
    *out = 444;
    return;
  }
  const int hratio = img.max_h / img.comp[0].h_samp;
  const int vratio = img.max_v / img.comp[0].v_samp;
  if (hratio == 2 && vratio == 2) {
    *out = 420;
  } else if (hratio == 2 && vratio == 1) {
    *out = 422;
  } else {
    *out = 444;
  }
}

// 编码前的帧描述：宽高、分量数、采样因子、量化表与标准 Huffman 表。
void describeFrame(int width, int height, int channels, int subsampling,
                   JpegImage *img) {
  img->width = width;
  img->height = height;
  img->ncomp = channels;
  img->valid = true;

  img->comp[0].id = 1;
  img->comp[0].tq = 0;
  img->comp[0].td = 0;
  img->comp[0].ta = 0;
  if (channels == 3) {
    img->comp[1].id = 2;
    img->comp[2].id = 3;
    img->comp[1].tq = 1;
    img->comp[2].tq = 1;
    img->comp[1].td = 1;
    img->comp[1].ta = 1;
    img->comp[2].td = 1;
    img->comp[2].ta = 1;
  }

  if (channels == 1 || subsampling == 444) {
    // 灰度没有色度可下采样；4:4:4 三分量同理。
    img->max_h = img->max_v = 1;
    for (int c = 0; c < channels; ++c) {
      img->comp[c].h_samp = img->comp[c].v_samp = 1;
    }
  } else if (subsampling == 420) {
    img->max_h = img->max_v = 2;
    img->comp[0].h_samp = 2;
    img->comp[0].v_samp = 2;
    img->comp[1].h_samp = img->comp[1].v_samp = 1;
    img->comp[2].h_samp = img->comp[2].v_samp = 1;
  } else { // 422
    img->max_h = 2;
    img->max_v = 1;
    img->comp[0].h_samp = 2;
    img->comp[0].v_samp = 1;
    img->comp[1].h_samp = img->comp[1].v_samp = 1;
    img->comp[2].h_samp = img->comp[2].v_samp = 1;
  }
}

} // namespace

bool decodeJpeg(const uint8_t *data, size_t size, const JpegOptions &options,
                ImageBuffer *out, bool *gpu_used, std::string *err) {
  if (gpu_used != nullptr) *gpu_used = false;
  if (out == nullptr || data == nullptr || size == 0) {
    if (err != nullptr) *err = "JPEG: null input";
    return false;
  }
  out->data.clear();

  JpegImage img;
  std::string local;
  std::vector<uint8_t> file(data, data + size);
  if (jpeg_parse(file, &img, &local) != 0) {
    if (err != nullptr) *err = "JPEG: parse failed: " + local;
    return false;
  }

  std::vector<float> coeffs;
  if (jpeg_entropy_decode(img, &coeffs, &local) != 0) {
    if (err != nullptr) *err = "JPEG: entropy decode failed: " + local;
    return false;
  }

  std::vector<uint8_t> pixels;
  const int channels = (img.ncomp == 1) ? 1 : 3;
  if (options.use_gpu) {
    DctOptions dct;
    dct.device_index = options.device_index;
    dct.memory_limit = options.memory_limit;
    if (gpuDecode(img, coeffs, dct, &pixels, &local)) {
      if (gpu_used != nullptr) *gpu_used = true;
    } else if (err != nullptr) {
      // 设备不可用 / 显存不足：如实报错，由路由层决定是否换后端，
      // 不在这里悄悄改用 CPU 再声称成功（那会让「后端到底做了什么」失真）。
      *err = "JPEG: GPU decode failed: " + local;
      return false;
    }
  } else if (!cpuDecode(img, coeffs, &pixels, &local)) {
    if (err != nullptr) *err = "JPEG: CPU decode failed: " + local;
    return false;
  }

  out->width = img.width;
  out->height = img.height;
  out->channels = channels;
  out->data = std::move(pixels);
  if (!out->valid()) {
    if (err != nullptr) *err = "JPEG: decoded image is inconsistent";
    return false;
  }
  return true;
}

bool encodeJpeg(const ImageBuffer &img, const JpegOptions &options,
                std::vector<uint8_t> *out, bool *gpu_used, std::string *err) {
  if (gpu_used != nullptr) *gpu_used = false;
  if (out == nullptr) {
    if (err != nullptr) *err = "JPEG: null output";
    return false;
  }
  out->clear();
  if (!img.valid()) {
    if (err != nullptr) *err = "JPEG: invalid image buffer";
    return false;
  }
  // JPEG 没有 Alpha 样本：2/4 通道必须显式拒绝（"unsupported" 让路由层把它
  // 分类成 UnsupportedFormat 并回退），绝不静默丢掉 Alpha 通道。
  if (img.channels != 1 && img.channels != 3) {
    if (err != nullptr) {
      *err = "JPEG: unsupported channel count " + std::to_string(img.channels) +
             " (only 1 or 3)";
    }
    return false;
  }

  int quality = options.quality;
  if (quality < 1) quality = 1;
  if (quality > 100) quality = 100;

  int subsampling = options.subsampling;
  if (img.channels == 1) {
    subsampling = 444; // 灰度没有色度
  } else if (subsampling != 444 && subsampling != 420 && subsampling != 422) {
    subsampling = (quality >= 92) ? 444 : 420;
  }

  JpegImage frame;
  describeFrame(img.width, img.height, img.channels, subsampling, &frame);
  jpeg_compute_layout(&frame);
  jpeg_make_quant_tables(quality, frame.quant);
  frame.huff_dc[0] = jpeg_std_dc_lum();
  frame.huff_ac[0] = jpeg_std_ac_lum();
  if (img.channels == 3) {
    frame.huff_dc[1] = jpeg_std_dc_chroma();
    frame.huff_ac[1] = jpeg_std_ac_chroma();
  }

  std::vector<float> coeffs;
  std::string local;
  if (options.use_gpu) {
    DctOptions dct;
    dct.device_index = options.device_index;
    dct.memory_limit = options.memory_limit;
    if (!gpuEncode(frame, img.data.data(), dct, &coeffs, &local)) {
      if (err != nullptr) *err = "JPEG: GPU encode failed: " + local;
      return false;
    }
    if (gpu_used != nullptr) *gpu_used = true;
  } else if (!cpuEncode(frame, img.data.data(), &coeffs, &local)) {
    if (err != nullptr) *err = "JPEG: CPU encode failed: " + local;
    return false;
  }

  std::vector<uint8_t> entropy;
  if (jpeg_entropy_encode(frame, coeffs, &entropy, &local) != 0) {
    if (err != nullptr) *err = "JPEG: entropy encode failed: " + local;
    return false;
  }
  if (jpeg_write_file(frame, entropy, out, &local) != 0) {
    if (err != nullptr) *err = "JPEG: container write failed: " + local;
    return false;
  }
  return true;
}

} // namespace jpeg
} // namespace vulkan
} // namespace image_codec
