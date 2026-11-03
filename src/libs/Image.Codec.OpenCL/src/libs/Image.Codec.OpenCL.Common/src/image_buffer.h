/*
 * File: image_buffer.h
 * Brief: Image buffer.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// image_buffer.h - 各编解码模块共用的宿主内存图像缓冲（top-down 行序）。
//
// 为什么要有这一层：OpenCL 后端的三个格式（TIFF / PNG / JPEG）以前各自揣着一份
// 自己的像素容器（BGR24 / RGB24 / 裸 vector），通道数一变就要改三处。统一成
// 一个「width x height x channels，第 0 行为图像顶部」的结构后：
//   * 通道保真变成可检查的硬约束（channels 是结构体字段，不是隐含约定）；
//   * GPU 侧的上传/回读只有一套尺寸推导（stride = width * channels）。
//
// 支持 channels 1..4，语义与 PNG 的颜色类型一一对应：
//   1 = 灰度、2 = 灰度+Alpha、3 = RGB、4 = RGBA。
// 为什么必须有 4：PNG 的真实世界里带 Alpha 的图（截图、图标、切图）比纯 RGB
// 还多，只支持 1/3 等于对一大类文件直接说「不支持」，然后被迫整条链路回退 CPU。
// Alpha 在这里是**原始样本**，不做预乘、不做合成——本库不是渲染器。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace image_codec {

struct ImageBuffer {
  int width = 0;
  int height = 0;
  int channels = 0;          // 1 = 灰度, 2 = 灰度+A, 3 = RGB, 4 = RGBA
  std::vector<uint8_t> data; // top-down，width*channels 字节/行

  // 每行字节数（不含 padding）。
  size_t stride() const {
    return static_cast<size_t>(width) * static_cast<size_t>(channels);
  }

  // 像素总数（字节）。
  size_t size() const { return stride() * static_cast<size_t>(height); }

  // 通道数是否被本库承认（1..4）。**注意**：承认通道不等于每个格式都能写，
  // 例如 JPEG 没有 Alpha，2/4 通道必须由调用方显式拒绝，不能悄悄降成 1/3。
  static bool validChannels(int c) { return c >= 1 && c <= 4; }

  bool valid() const {
    return width > 0 && height > 0 && validChannels(channels) &&
           data.size() >= size();
  }

  // 按尺寸/通道分配（保留内容不做保证，调用方自己填）。
  bool allocate(int w, int h, int c) {
    if (w <= 0 || h <= 0 || !validChannels(c)) {
      return false;
    }
    width = w;
    height = h;
    channels = c;
    data.assign(static_cast<size_t>(w) * static_cast<size_t>(c) *
                    static_cast<size_t>(h),
                0);
    return true;
  }

  void reset() {
    width = 0;
    height = 0;
    channels = 0;
    data.clear();
  }
};

} // namespace image_codec
