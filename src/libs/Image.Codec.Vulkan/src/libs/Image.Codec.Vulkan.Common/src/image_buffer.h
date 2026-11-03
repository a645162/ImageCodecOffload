// File      : image_buffer.h
// Brief     : 宿主侧图像缓冲（top-down，1/2/3/4 通道）
// Author    : 孔昊旻(Haomin Kong)
// Email     : a645162@163.com
// Created   : 2026-10-31
//
// 与 Image.Codec.CUDA / Image.Codec.OpenCL 的 ImageBuffer 语义一致：
//   * 行序 top-down，第 0 行是图像顶部；
//   * channels 就是真实样本数（1 = 灰度、2 = 灰度+Alpha、3 = RGB、4 = RGBA，
//     Alpha 非预乘），不做隐式升维，也不丢 Alpha；
//   * 紧凑布局：stride == width * channels。
// 三个后端的通道语义必须完全一致，否则「同一张图换后端」会悄悄换掉字节数。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace image_codec {
namespace vulkan {

struct ImageBuffer {
  int width = 0;
  int height = 0;
  int channels = 0;
  std::vector<uint8_t> data;

  size_t stride() const {
    return static_cast<size_t>(width) * static_cast<size_t>(channels);
  }

  size_t bytes() const { return stride() * static_cast<size_t>(height); }

  bool valid() const {
    return width > 0 && height > 0 &&
           (channels == 1 || channels == 2 || channels == 3 || channels == 4) &&
           data.size() == bytes();
  }

  // 按 (w, h, c) 分配并清零；参数非法返回 false。
  bool allocate(int w, int h, int c) {
    if (w <= 0 || h <= 0 || (c != 1 && c != 2 && c != 3 && c != 4))
      return false;
    width = w;
    height = h;
    channels = c;
    data.assign(static_cast<size_t>(w) * static_cast<size_t>(h) *
                    static_cast<size_t>(c),
                0);
    return true;
  }
};

} // namespace vulkan
} // namespace image_codec
