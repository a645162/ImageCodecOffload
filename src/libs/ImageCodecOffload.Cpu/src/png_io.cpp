/*
 * File: png_io.cpp
 * Brief: Png io.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// png_io.cpp - libpng 的编解码基线。
//
// libpng 的错误模型是 setjmp/longjmp：任何一次读写在内部失败都会跳到我们
// 设置的 jmpbuf。因此每个入口都必须先 setjmp 再做任何调用，并且保证跳回来
// 之后把 png_struct/png_info 销毁干净——否则一次坏输入就会漏一块结构。
#include "png_io.h"

// zlib 的压缩策略常量（Z_*_STRATEGY）只由 zlib.h 提供。libpng 在部分配置下
// 不把 zlib.h 带进来（它自己的 PNG_Z_* 宏只在 pnglibconf 里），所以这里显式
// 包含，而不是依赖 png.h 的传递包含。
#include <zlib.h>

#include <png.h>

#include <csetjmp>
#include <cstring>
#include <memory>
#include <string>

namespace image_codec {
namespace offload {
namespace cpu {

namespace {

// ---- 写内存 ----
struct PngSink {
  std::vector<uint8_t> *out = nullptr;
};

void pngWriteMemory(png_structp png, png_bytep data, png_size_t length) {
  auto *sink = static_cast<PngSink *>(png_get_io_ptr(png));
  if (sink == nullptr || sink->out == nullptr) return;
  sink->out->insert(sink->out->end(), data, data + length);
}

void pngFlushMemory(png_structp) {}

int filterMask(int filter) {
  switch (filter) {
  case 0:
    return PNG_FILTER_NONE;
  case 1:
    return PNG_FILTER_SUB;
  case 2:
    return PNG_FILTER_UP;
  case 3:
    return PNG_FILTER_AVG;
  case 4:
    return PNG_FILTER_PAETH;
  default:
    return PNG_ALL_FILTERS; // -1 = 自适应
  }
}

// zlib 的压缩策略。它不是「压缩级别」的同义词：级别决定多努力找匹配，
// 策略决定「允许用哪类匹配」。RLE 与 Huffman-only 在特定内容上明显更快/更小，
// 但也可能完全压不动，所以只能由调用方显式选。
int strategyValue(int strategy) {
  switch (strategy) {
  case 0:
    return Z_DEFAULT_STRATEGY;
  case 1:
    return Z_FILTERED;
  case 2:
    return Z_HUFFMAN_ONLY;
  case 3:
    return Z_RLE;
  case 4:
    return Z_FIXED;
  default:
    return Z_DEFAULT_STRATEGY;
  }
}

} // namespace

Result<std::vector<uint8_t>> encodePng(const Image &image,
                                       const EncodeParams &params) {
  if (!image.valid()) {
    return fail<std::vector<uint8_t>>(Status::InvalidArgument,
                                      "png: invalid image");
  }
  // PNG 的四种颜色类型正好对应 1/2/3/4 通道：
  //   1 = 灰度、2 = 灰度+Alpha、3 = RGB、4 = RGBA。
  // 少写一种就是把「带 Alpha 的 PNG」整类交给别的后端——而这类图最常见。
  if (image.channels < 1 || image.channels > 4) {
    return fail<std::vector<uint8_t>>(Status::UnsupportedFormat,
                                      "png: unsupported channel count");
  }

  std::vector<uint8_t> out;
  PngSink sink;
  sink.out = &out;

  png_structp png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) {
    return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                      "png: create write struct failed");
  }

  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_write_struct(&png, nullptr);
    return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                      "png: create info struct failed");
  }

  if (setjmp(png_jmpbuf(png)) != 0) {
    png_destroy_write_struct(&png, &info);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "png: libpng reported an error");
  }

  png_set_write_fn(png, &sink, pngWriteMemory, pngFlushMemory);
  static const int kColorType[5] = {
      0, PNG_COLOR_TYPE_GRAY, PNG_COLOR_TYPE_GRAY_ALPHA, PNG_COLOR_TYPE_RGB,
      PNG_COLOR_TYPE_RGB_ALPHA};
  png_set_IHDR(png, info, static_cast<png_uint_32>(image.width),
               static_cast<png_uint_32>(image.height), 8,
               kColorType[image.channels], PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  png_set_compression_level(
      png, params.png_compression < 0
               ? 6
               : (params.png_compression > 9 ? 9 : params.png_compression));
  png_set_filter(png, 0, filterMask(params.png_filter));
  // -1 = 不干预（libpng 默认即 Z_DEFAULT_STRATEGY）。0 也是默认策略，
  // 所以显式给 0 与不写等价——这一点在 C API 注释里已写明。
  if (params.png_strategy >= 0) {
    png_set_compression_strategy(png, strategyValue(params.png_strategy));
  }

  png_write_info(png, info);

  for (int y = 0; y < image.height; ++y) {
    png_bytep row = const_cast<png_bytep>(
        image.pixels.data() + static_cast<size_t>(y) * image.rowBytes());
    png_write_row(png, row);
  }
  png_write_end(png, nullptr);

  png_destroy_write_struct(&png, &info);
  return out;
}

Result<Image> decodePng(const uint8_t *data, size_t size) {
  if (data == nullptr || size == 0) {
    return fail<Image>(Status::InvalidArgument, "png: empty input");
  }

  png_image img;
  std::memset(&img, 0, sizeof(img));
  img.version = PNG_IMAGE_VERSION;

  if (png_image_begin_read_from_memory(&img, data, size) == 0) {
    return fail<Image>(Status::DecodeFailed,
                       std::string("png: ") + img.message);
  }

  // 通道保真：原图有几个样本就输出几个，绝不把 Alpha 丢掉或把灰度升维。
  // 简化 API 里 PNG_FORMAT_FLAG_ALPHA 就是「多一个 Alpha 样本」，
  // 因此 color/alpha 两个布尔量直接决定 1/2/3/4 通道。
  const bool color =
      (img.format & (PNG_FORMAT_FLAG_COLOR | PNG_FORMAT_FLAG_COLORMAP)) != 0;
  const bool alpha = (img.format & PNG_FORMAT_FLAG_ALPHA) != 0;
  if (color && alpha) {
    img.format = PNG_FORMAT_RGBA;
  } else if (color) {
    img.format = PNG_FORMAT_RGB;
  } else if (alpha) {
    img.format = PNG_FORMAT_GA;
  } else {
    img.format = 0;
  }

  Image out;
  out.width = static_cast<int>(img.width);
  out.height = static_cast<int>(img.height);
  out.channels = (color ? 3 : 1) + (alpha ? 1 : 0);
  out.pixels.resize(PNG_IMAGE_SIZE(img));

  if (png_image_finish_read(&img, nullptr, out.pixels.data(), 0, nullptr) ==
      0) {
    png_image_free(&img);
    return fail<Image>(Status::DecodeFailed,
                       std::string("png: ") + img.message);
  }

  png_image_free(&img);
  return out;
}

Result<int> probePngChannels(const uint8_t *data, size_t size) {
  // 签名(8) + IHDR 长度(4) + "IHDR"(4) + width(4) + height(4) + bitdepth(1)
  //   + colortype(1) —— 共 26 字节即可回答。
  static const uint8_t kSignature[8] = {0x89, 'P',  'N',  'G',
                                        0x0D, 0x0A, 0x1A, 0x0A};
  if (data == nullptr || size < 26 ||
      std::memcmp(data, kSignature, sizeof(kSignature)) != 0) {
    return fail<int>(Status::UnsupportedFormat, "png: bad signature");
  }
  if (std::memcmp(data + 12, "IHDR", 4) != 0) {
    return fail<int>(Status::UnsupportedFormat, "png: IHDR not first");
  }

  // color type：0 = 灰度，2 = RGB，3 = 调色板，4 = 灰度+alpha，6 = RGBA。
  // 调色板按 3 通道算：解出来就是彩色，不是「原图是灰度」。
  const uint8_t color_type = data[25];
  return (color_type == 0 || color_type == 4) ? 1 : 3;
}

} // namespace cpu
} // namespace offload
} // namespace image_codec
