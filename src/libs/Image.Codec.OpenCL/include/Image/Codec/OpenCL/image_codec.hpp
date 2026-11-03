/*
 * File: image_codec.hpp
 * Brief: Image codec.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// image_codec.hpp - OpenCL-Image-Codec C++ RAII 包装（纯头，基于 image_codec.h）。
//
// 用法：
//   #include <Image/Codec/OpenCL/image_codec.hpp>
//   image_codec::Codec codec(IMAGE_CODEC_BACKEND_OPENCL);
//   image_codec::Image img = codec.decode(IMAGE_CODEC_FORMAT_JPEG, data, size);
//   std::vector<uint8_t> out = codec.encode(IMAGE_CODEC_FORMAT_TIFF, img, params);
//
// 便捷重载抛 image_codec::Error；原始重载返回 image_codec_status（不抛异常）。
#pragma once

#include "image_codec.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace image_codec {

// ---- 异常类型 ----
// 将 image_codec_status 映射为 C++ 异常（what() 为 image_codec_status_string 文本）。
class Error : public std::runtime_error {
 public:
  explicit Error(image_codec_status status)
      : std::runtime_error(image_codec_status_string(status)), status_(status) {}
  image_codec_status status() const noexcept { return status_; }

 private:
  image_codec_status status_;
};

// image_codec_status -> 可读字符串。
inline const char *StatusString(image_codec_status status) noexcept {
  return image_codec_status_string(status);
}

// 默认编码参数：JPEG quality=85，TIFF LZW 压缩、rows_per_strip 自动。
inline image_codec_encode_params default_encode_params() noexcept {
  image_codec_encode_params p{};
  p.jpeg_quality = 85;
  p.tiff_compression = 5;
  p.tiff_rows_per_strip = 0;
  return p;
}

// ---- image_codec_image RAII 容器（移动语义；拷贝禁用）----
class Image {
 public:
  Image() noexcept : img_() {}
  ~Image() { image_codec_image_free(&img_); }

  Image(const Image &) = delete;
  Image &operator=(const Image &) = delete;

  Image(Image &&other) noexcept : img_(other.img_) {
    other.img_ = image_codec_image{};
  }
  Image &operator=(Image &&other) noexcept {
    if (this != &other) {
      image_codec_image_free(&img_);
      img_ = other.img_;
      other.img_ = image_codec_image{};
    }
    return *this;
  }

  // 用外部数据构造（RGB top-down）。owned_data 必须为 malloc 分配，
  // 转移所有权：析构时以 image_codec_image_free 释放。stride 为行字节数。
  Image(uint32_t width, uint32_t height, uint32_t channels, uint8_t *owned_data,
        size_t stride, size_t data_size) noexcept
      : img_{width, height, channels, owned_data, stride, data_size} {}

  uint32_t width() const noexcept { return img_.width; }
  uint32_t height() const noexcept { return img_.height; }
  uint32_t channels() const noexcept { return img_.channels; }
  size_t stride() const noexcept { return img_.stride; }
  size_t data_size() const noexcept { return img_.data_size; }
  const uint8_t *data() const noexcept { return img_.data; }
  uint8_t *data() noexcept { return img_.data; }

  image_codec_image *native() noexcept { return &img_; }
  const image_codec_image *native() const noexcept { return &img_; }

 private:
  image_codec_image img_;
};

// ---- 编解码器 RAII 句柄 ----
class Codec {
 public:
  explicit Codec(image_codec_backend backend) : codec_(nullptr) {
    const image_codec_status s = image_codec_codec_create(&codec_, backend);
    if (s != IMAGE_CODEC_OK) {
      throw Error(s);
    }
  }
  ~Codec() {
    if (codec_ != nullptr) {
      image_codec_codec_destroy(codec_);
    }
  }

  Codec(const Codec &) = delete;
  Codec &operator=(const Codec &) = delete;

  Codec(Codec &&other) noexcept : codec_(other.codec_) {
    other.codec_ = nullptr;
  }
  Codec &operator=(Codec &&other) noexcept {
    if (this != &other) {
      if (codec_ != nullptr) {
        image_codec_codec_destroy(codec_);
      }
      codec_ = other.codec_;
      other.codec_ = nullptr;
    }
    return *this;
  }

  // 解码：失败抛 image_codec::Error；成功返回 Image。
  Image decode(image_codec_format format, const uint8_t *in, size_t in_len) {
    Image img;
    const image_codec_status s =
        image_codec_codec_decode(codec_, format, in, in_len, img.native());
    if (s != IMAGE_CODEC_OK) {
      throw Error(s);
    }
    return img;
  }

  // 解码：返回 image_codec_status（不抛异常）。out 由调用方持有并在 image_codec_image_free 释放。
  image_codec_status decode(image_codec_format format, const uint8_t *in, size_t in_len,
                    image_codec_image *out) noexcept {
    return image_codec_codec_decode(codec_, format, in, in_len, out);
  }

  // 编码：自动扩容输出缓冲，失败抛 image_codec::Error。接受 image_codec::Image 或原生 image_codec_image。
  std::vector<uint8_t> encode(image_codec_format format, const image_codec_image &img,
                              const image_codec_encode_params &params =
                                  default_encode_params()) {
    std::vector<uint8_t> buf(1u << 16);  // 64KB 起
    for (;;) {
      size_t out_len = 0;
      const image_codec_status s =
          image_codec_codec_encode(codec_, format, &img, &params, buf.data(),
                           &out_len, buf.size());
      if (s == IMAGE_CODEC_OK) {
        buf.resize(out_len);
        return buf;
      }
      if (s == IMAGE_CODEC_ERR_OUT_OF_MEMORY && out_len > buf.size()) {
        buf.resize(out_len);
        continue;
      }
      throw Error(s);
    }
  }
  std::vector<uint8_t> encode(image_codec_format format, const Image &img,
                              const image_codec_encode_params &params =
                                  default_encode_params()) {
    return encode(format, *img.native(), params);
  }

  // 编码：返回 image_codec_status（不抛异常）。
  image_codec_status encode(image_codec_format format, const image_codec_image &img,
                    const image_codec_encode_params &params, uint8_t *out,
                    size_t *out_len, size_t out_capacity) noexcept {
    return image_codec_codec_encode(codec_, format, &img, &params, out, out_len,
                            out_capacity);
  }

  image_codec_codec *native() const noexcept { return codec_; }

 private:
  image_codec_codec *codec_;
};

// ---- OpenCL 设备枚举 RAII ----
class DeviceList {
 public:
  DeviceList() : names_(nullptr), count_(0) {
    const image_codec_status s = image_codec_device_list(&names_, &count_);
    if (s != IMAGE_CODEC_OK) {
      throw Error(s);
    }
  }
  ~DeviceList() {
    if (names_ != nullptr) {
      image_codec_device_list_free(names_, count_);
    }
  }

  DeviceList(const DeviceList &) = delete;
  DeviceList &operator=(const DeviceList &) = delete;

  int size() const noexcept { return count_; }
  bool empty() const noexcept { return count_ <= 0; }
  const char *name(int index) const noexcept {
    return (index >= 0 && index < count_) ? names_[index] : nullptr;
  }

 private:
  char **names_;
  int count_;
};

}  // namespace image_codec
