/*
 * File: container_probe.cpp
 * Brief: Container probe.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// container_probe.cpp - 按格式分派到各自的「只读头」探测实现。
//
// 这里刻意只做分派：每种容器的字段位置与坑都在它自己的 *_io.cpp 里，集中写
// 一份反而会让「JPEG 的 SOF 排除 DHT」「调色板算彩色」这类知识离开它该在的
// 地方，两个地方各改一半就漂移了。
#include "container_probe.h"

#include "jpeg_io.h"
#include "png_io.h"
#include "tiff_io.h"

namespace image_codec {
namespace offload {
namespace cpu {

Result<int> probeChannels(Format format, const uint8_t* data, size_t size) {
  switch (format) {
    case Format::Jpeg:
      return probeJpegChannels(data, size);
    case Format::Png:
      return probePngChannels(data, size);
    case Format::Tiff:
      return probeTiffChannels(data, size);
    case Format::Jpeg2000:
    case Format::JpegXl:
      return fail<int>(Status::UnsupportedFormat,
                       "probe: extended format channel probe unavailable");
    case Format::Unknown:
      break;
  }
  return fail<int>(Status::UnsupportedFormat, "probe: unknown format");
}

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
