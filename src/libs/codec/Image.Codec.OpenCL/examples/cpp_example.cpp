/*
 * File: cpp_example.cpp
 * Brief: Cpp example.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// examples/cpp_example.cpp - C++ 示例：使用 image_codec.hpp RAII 包装。
//
// 用法：image_codec_example_cpp <in.bmp> <out-prefix>
//   与 c_example.c 等价，但全程 RAII/异常，无需手动释放。
#include <Image/Codec/OpenCL/image_codec.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// 最小 24-bit BMP 读取（BGR -> RGB，top-down 行序）。
bool load_bmp(const char *path, std::vector<uint8_t> *rgb, uint32_t *width,
              uint32_t *height) {
  std::vector<uint8_t> file;
  {
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); return false; }
    file.resize(static_cast<size_t>(sz));
    if (sz > 0 &&
        std::fread(file.data(), 1, file.size(), f) != file.size()) {
      std::fclose(f);
      return false;
    }
    std::fclose(f);
  }
  if (file.size() < 54 || file[0] != 'B' || file[1] != 'M') return false;
  auto u16 = [&](size_t o) -> uint16_t {
    return static_cast<uint16_t>(file[o] | (file[o + 1] << 8));
  };
  auto u32 = [&](size_t o) -> uint32_t {
    return static_cast<uint32_t>(file[o] | (file[o + 1] << 8) |
                                 (file[o + 2] << 16) | (file[o + 3] << 24));
  };
  const uint32_t off = u32(10);
  const uint32_t dib = u32(14);
  int w = 0, h = 0, bpp = 0;
  bool top_down = false;
  if (dib == 40) {
    const int32_t iw = static_cast<int32_t>(u32(18));
    const int32_t ih = static_cast<int32_t>(u32(22));
    w = iw < 0 ? -iw : iw;
    h = ih < 0 ? -ih : ih;
    top_down = ih < 0;
    bpp = u16(28);
    if (u32(30) != 0) return false;
  } else if (dib == 12) {
    w = u16(18);
    h = u16(20);
    bpp = u16(24);
  } else {
    return false;
  }
  if (w <= 0 || h <= 0 || bpp != 24) return false;
  const size_t row_stride = ((size_t)w * 3 + 3) / 4 * 4;
  if (off + row_stride * (size_t)h > file.size()) return false;
  rgb->assign((size_t)w * h * 3, 0);
  for (int y = 0; y < h; y++) {
    const int src_row = top_down ? y : (h - 1 - y);
    const uint8_t *src = file.data() + off + (size_t)src_row * row_stride;
    uint8_t *dst = rgb->data() + (size_t)y * (size_t)w * 3;
    for (int x = 0; x < w; x++) {
      dst[x * 3 + 0] = src[x * 3 + 2];
      dst[x * 3 + 1] = src[x * 3 + 1];
      dst[x * 3 + 2] = src[x * 3 + 0];
    }
  }
  *width = static_cast<uint32_t>(w);
  *height = static_cast<uint32_t>(h);
  return true;
}

bool write_file(const char *path, const uint8_t *data, size_t size) {
  FILE *f = std::fopen(path, "wb");
  if (f == nullptr) return false;
  const size_t n = std::fwrite(data, 1, size, f);
  std::fclose(f);
  return n == size;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: image_codec_example_cpp <in.bmp> <out-prefix>\n");
    return 2;
  }
  try {
    /* 1. 枚举 OpenCL 设备（RAII，析构自动释放） */
    image_codec::DeviceList devs;
    std::printf("%d OpenCL device(s):\n", devs.size());
    for (int i = 0; i < devs.size(); i++) {
      std::printf("  [%d] %s\n", i, devs.name(i));
    }

    /* 2. 读 BMP -> image_codec::Image（数据转移给 Image 管理） */
    std::vector<uint8_t> rgb;
    uint32_t w = 0, h = 0;
    if (!load_bmp(argv[1], &rgb, &w, &h)) {
      std::fprintf(stderr, "cannot load BMP: %s\n", argv[1]);
      return 1;
    }
    uint8_t *px = static_cast<uint8_t *>(std::malloc(rgb.size()));
    if (px == nullptr) {
      std::fprintf(stderr, "out of memory\n");
      return 1;
    }
    std::memcpy(px, rgb.data(), rgb.size());
    image_codec::Image img(w, h, 3, px, (size_t)w * 3, rgb.size());
    std::printf("loaded %s: %ux%u (%zu bytes)\n", argv[1], w, h, rgb.size());

    /* 3. 编解码器 + 编码参数 */
    image_codec::Codec codec(IMAGE_CODEC_BACKEND_OPENCL);
    image_codec_encode_params params = image_codec::default_encode_params();
    // params.jpeg_quality = 85; tiff_compression = 5; rows_per_strip = 0。

    /* 4a. 编码 TIFF */
    {
      std::vector<uint8_t> tif = codec.encode(IMAGE_CODEC_FORMAT_TIFF, img, params);
      const std::string path = std::string(argv[2]) + ".tif";
      if (!write_file(path.c_str(), tif.data(), tif.size())) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
      }
      std::printf("encoded TIFF -> %s (%zu bytes)\n", path.c_str(),
                  tif.size());
    }

    /* 4b. 编码 JPEG */
    std::vector<uint8_t> jpg;
    {
      jpg = codec.encode(IMAGE_CODEC_FORMAT_JPEG, img, params);
      const std::string path = std::string(argv[2]) + ".jpg";
      if (!write_file(path.c_str(), jpg.data(), jpg.size())) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
      }
      std::printf("encoded JPEG -> %s (%zu bytes)\n", path.c_str(),
                  jpg.size());
    }

    /* 5. 解码回读并校验尺寸 */
    {
      image_codec::Image dec = codec.decode(IMAGE_CODEC_FORMAT_JPEG, jpg.data(), jpg.size());
      std::printf("decoded -> %ux%u channels=%u (%zu bytes)\n", dec.width(),
                  dec.height(), dec.channels(), dec.data_size());
      if (dec.width() != w || dec.height() != h) {
        std::fprintf(stderr, "size mismatch after decode\n");
        return 1;
      }
    }

    /* 6. 错误路径：垃圾数据解码应抛 image_codec::Error 而非崩溃 */
    {
      static const uint8_t junk[16] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4,
                                       5,    6,    7,    8,    9, 10, 11, 12};
      try {
        image_codec::Image bad =
            codec.decode(IMAGE_CODEC_FORMAT_JPEG, junk, sizeof(junk));
        std::fprintf(stderr, "expected image_codec::Error for garbage input\n");
        (void)bad;
        return 1;
      } catch (const image_codec::Error &e) {
        std::printf("error path: decode(garbage) -> %s (%d)\n", e.what(),
                    static_cast<int>(e.status()));
      }
    }

    return 0;
  } catch (const image_codec::Error &e) {
    std::fprintf(stderr, "image_codec error: %s (%d)\n", e.what(),
                 static_cast<int>(e.status()));
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "exception: %s\n", e.what());
    return 1;
  }
}
