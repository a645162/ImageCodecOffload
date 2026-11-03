/*
 * File: c_example.c
 * Brief: C example.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// examples/c_example.c - 纯 C 示例：使用 image_codec.h C API。
//
// 用法：image_codec_example_c <in.bmp> <out-prefix>
//   1. 枚举 OpenCL 设备
//   2. 读取 24-bit BMP 为 image_codec_image
//   3. 编码 TIFF / JPEG（OpenCL 后端）写盘
//   4. 读回 JPEG 解码并校验尺寸
//   5. 错误路径：对垃圾数据解码应返回明确错误码而不崩溃
#include <Image/Codec/OpenCL/image_codec.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 最小 24-bit BMP 读取（BGR -> RGB，top-down 行序）---- */
static int load_bmp(const char *path, uint8_t **data, size_t *size,
                    uint32_t *width, uint32_t *height) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long flen = ftell(f);
  if (fseek(f, 0, SEEK_SET) != 0 || flen < 54) { fclose(f); return -1; }
  uint8_t *buf = (uint8_t *)malloc((size_t)flen);
  if (buf == NULL) { fclose(f); return -1; }
  if (fread(buf, 1, (size_t)flen, f) != (size_t)flen) {
    free(buf); fclose(f); return -1;
  }
  fclose(f);

  if (buf[0] != 'B' || buf[1] != 'M') { free(buf); return -1; }
  const uint32_t off = (uint32_t)buf[10] | ((uint32_t)buf[11] << 8) |
                       ((uint32_t)buf[12] << 16) | ((uint32_t)buf[13] << 24);
  const uint32_t dib = (uint32_t)buf[14] | ((uint32_t)buf[15] << 8) |
                       ((uint32_t)buf[16] << 16) | ((uint32_t)buf[17] << 24);
  int w, h, bpp, top_down;
  if (dib == 40) {
    const int32_t iw = (int32_t)((uint32_t)buf[18] | ((uint32_t)buf[19] << 8) |
                                 ((uint32_t)buf[20] << 16) | ((uint32_t)buf[21] << 24));
    const int32_t ih = (int32_t)((uint32_t)buf[22] | ((uint32_t)buf[23] << 8) |
                                 ((uint32_t)buf[24] << 16) | ((uint32_t)buf[25] << 24));
    const uint32_t comp = (uint32_t)buf[30] | ((uint32_t)buf[31] << 8) |
                          ((uint32_t)buf[32] << 16) | ((uint32_t)buf[33] << 24);
    w = iw < 0 ? -iw : iw;
    h = ih < 0 ? -ih : ih;
    bpp = (int)(uint16_t)(buf[28] | (buf[29] << 8));
    top_down = ih < 0;
    if (comp != 0) { free(buf); return -1; }
  } else if (dib == 12) {
    w = (int)(buf[18] | (buf[19] << 8));
    h = (int)(buf[20] | (buf[21] << 8));
    bpp = (int)(buf[24] | (buf[25] << 8));
    top_down = 0;
  } else {
    free(buf);
    return -1;
  }
  if (w <= 0 || h <= 0 || bpp != 24) { free(buf); return -1; }

  const size_t row_stride = ((size_t)w * 3 + 3) / 4 * 4;
  if (off + row_stride * (size_t)h > (size_t)flen) { free(buf); return -1; }
  const size_t px = (size_t)w * (size_t)h * 3;
  uint8_t *rgb = (uint8_t *)malloc(px);
  if (rgb == NULL) { free(buf); return -1; }
  for (int y = 0; y < h; y++) {
    const int src_row = top_down ? y : (h - 1 - y);
    const uint8_t *src = buf + off + (size_t)src_row * row_stride;
    uint8_t *dst = rgb + (size_t)y * (size_t)w * 3;
    for (int x = 0; x < w; x++) {
      dst[x * 3 + 0] = src[x * 3 + 2];
      dst[x * 3 + 1] = src[x * 3 + 1];
      dst[x * 3 + 2] = src[x * 3 + 0];
    }
  }
  free(buf);
  *data = rgb;
  *size = px;
  *width = (uint32_t)w;
  *height = (uint32_t)h;
  return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) return -1;
  const size_t n = fwrite(data, 1, size, f);
  fclose(f);
  return n == size ? 0 : -1;
}

static int read_file(const char *path, uint8_t **data, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long flen = ftell(f);
  if (fseek(f, 0, SEEK_SET) != 0 || flen < 0) { fclose(f); return -1; }
  uint8_t *buf = (uint8_t *)malloc((size_t)flen);
  if (buf == NULL) { fclose(f); return -1; }
  if (flen > 0 && fread(buf, 1, (size_t)flen, f) != (size_t)flen) {
    free(buf); fclose(f); return -1;
  }
  fclose(f);
  *data = buf;
  *size = (size_t)flen;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: image_codec_example_c <in.bmp> <out-prefix>\n");
    return 2;
  }
  const char *in_bmp = argv[1];
  const char *prefix = argv[2];

  /* 1. 枚举 OpenCL 设备 */
  char **devs = NULL;
  int ndevs = 0;
  image_codec_status st = image_codec_device_list(&devs, &ndevs);
  if (st != IMAGE_CODEC_OK) {
    fprintf(stderr, "image_codec_device_list: %s\n", image_codec_status_string(st));
    return 1;
  }
  printf("%d OpenCL device(s):\n", ndevs);
  for (int i = 0; i < ndevs; i++) {
    printf("  [%d] %s\n", i, devs[i]);
  }
  image_codec_device_list_free(devs, ndevs);

  /* 2. 读 BMP -> image_codec_image */
  uint8_t *px = NULL;
  size_t px_size = 0;
  uint32_t w = 0, h = 0;
  if (load_bmp(in_bmp, &px, &px_size, &w, &h) != 0) {
    fprintf(stderr, "cannot load BMP: %s\n", in_bmp);
    return 1;
  }
  image_codec_image img;
  memset(&img, 0, sizeof(img));
  img.width = w;
  img.height = h;
  img.channels = 3;
  img.stride = (size_t)w * 3;
  img.data = px;
  img.data_size = px_size;
  printf("loaded %s: %ux%u (%zu bytes)\n", in_bmp, w, h, px_size);

  /* 3. 创建编解码器（OpenCL 后端） */
  image_codec_codec *codec = NULL;
  st = image_codec_codec_create(&codec, IMAGE_CODEC_BACKEND_OPENCL);
  if (st != IMAGE_CODEC_OK) {
    fprintf(stderr, "image_codec_codec_create: %s\n", image_codec_status_string(st));
    free(px);
    return 1;
  }

  image_codec_encode_params params;
  memset(&params, 0, sizeof(params));
  params.jpeg_quality = 85;
  params.tiff_compression = 5;      /* LZW */
  params.tiff_rows_per_strip = 16;

  static uint8_t out_buf[1u << 24]; /* 16MB 输出缓冲 */
  size_t out_len = 0;
  char out_path[1024];
  int failed = 0;

  /* 4a. 编码 TIFF */
  st = image_codec_codec_encode(codec, IMAGE_CODEC_FORMAT_TIFF, &img, &params, out_buf,
                        &out_len, sizeof(out_buf));
  if (st != IMAGE_CODEC_OK) {
    fprintf(stderr, "encode TIFF: %s\n", image_codec_status_string(st));
    failed = 1;
  } else {
    snprintf(out_path, sizeof(out_path), "%s.tif", prefix);
    if (write_file(out_path, out_buf, out_len) != 0) {
      fprintf(stderr, "cannot write %s\n", out_path);
      failed = 1;
    } else {
      printf("encoded TIFF -> %s (%zu bytes)\n", out_path, out_len);
    }
  }

  /* 4b. 编码 JPEG */
  st = image_codec_codec_encode(codec, IMAGE_CODEC_FORMAT_JPEG, &img, &params, out_buf,
                        &out_len, sizeof(out_buf));
  if (st != IMAGE_CODEC_OK) {
    fprintf(stderr, "encode JPEG: %s\n", image_codec_status_string(st));
    failed = 1;
  } else {
    snprintf(out_path, sizeof(out_path), "%s.jpg", prefix);
    if (write_file(out_path, out_buf, out_len) != 0) {
      fprintf(stderr, "cannot write %s\n", out_path);
      failed = 1;
    } else {
      printf("encoded JPEG -> %s (%zu bytes)\n", out_path, out_len);
    }
  }

  /* 5. 读回 JPEG 并解码校验 */
  if (!failed) {
    snprintf(out_path, sizeof(out_path), "%s.jpg", prefix);
    uint8_t *jpeg_data = NULL;
    size_t jpeg_size = 0;
    if (read_file(out_path, &jpeg_data, &jpeg_size) != 0) {
      fprintf(stderr, "cannot read %s\n", out_path);
      failed = 1;
    } else {
      image_codec_image dec;
      memset(&dec, 0, sizeof(dec));
      st = image_codec_codec_decode(codec, IMAGE_CODEC_FORMAT_JPEG, jpeg_data, jpeg_size, &dec);
      free(jpeg_data);
      if (st != IMAGE_CODEC_OK) {
        fprintf(stderr, "decode JPEG: %s\n", image_codec_status_string(st));
        failed = 1;
      } else {
        printf("decoded %s -> %ux%u channels=%u (%zu bytes)\n", out_path,
               dec.width, dec.height, dec.channels, dec.data_size);
        if (dec.width != w || dec.height != h) {
          fprintf(stderr, "size mismatch after decode\n");
          failed = 1;
        }
        image_codec_image_free(&dec);
      }
    }
  }

  /* 6. 错误路径：垃圾数据解码应返回明确错误码而非崩溃 */
  {
    static const uint8_t junk[16] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4,
                                     5,    6,    7,    8,    9, 10, 11, 12};
    image_codec_image bad;
    memset(&bad, 0, sizeof(bad));
    st = image_codec_codec_decode(codec, IMAGE_CODEC_FORMAT_JPEG, junk, sizeof(junk), &bad);
    printf("error path: decode(garbage) -> %s (%d)\n", image_codec_status_string(st),
           (int)st);
    if (st == IMAGE_CODEC_OK) {
      fprintf(stderr, "expected a non-OK status for garbage input\n");
      failed = 1;
      image_codec_image_free(&bad);
    }
  }

  image_codec_codec_destroy(codec);
  free(px);
  return failed ? 1 : 0;
}
