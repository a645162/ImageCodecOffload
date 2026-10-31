// jpeg_io.cpp - libjpeg-turbo 的 baseline JPEG 编解码基线。
//
// libjpeg 的错误模型同样是 setjmp/longjmp（jpeg_error_mgr::error_exit）。
// 默认的 error_exit 会直接 exit()——在库里这是不可接受的，必须换成抛到
// 我们自己 jmpbuf 的实现。
#include "jpeg_io.h"

#include <jpeglib.h>

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace image_codec {
namespace offload {
namespace cpu {

namespace {

// 把 libjpeg 的致命错误转成 longjmp，上层统一收成 DecodeFailed/EncodeFailed。
struct JpegError {
  jpeg_error_mgr pub;
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX];
};

void jpegErrorExit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<JpegError*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->message);
  longjmp(err->jump, 1);
}

// 警告不打断编解码（例如「corrupt data found」），但必须有个出口，
// 否则默认实现会往 stderr 打一堆字。
void jpegEmitMessage(j_common_ptr, int) {}

void applySubsampling(j_compress_ptr cinfo, int subsampling) {
  switch (subsampling) {
    case 444:
      cinfo->comp_info[0].h_samp_factor = 1;
      cinfo->comp_info[0].v_samp_factor = 1;
      break;
    case 422:
      cinfo->comp_info[0].h_samp_factor = 2;
      cinfo->comp_info[0].v_samp_factor = 1;
      break;
    case 420:
    default:
      cinfo->comp_info[0].h_samp_factor = 2;
      cinfo->comp_info[0].v_samp_factor = 2;
      break;
  }
}

}  // namespace

Result<std::vector<uint8_t>> encodeJpeg(const Image& image,
                                        const EncodeParams& params) {
  if (!image.valid()) {
    return fail<std::vector<uint8_t>>(Status::InvalidArgument,
                                      "jpeg: invalid image");
  }
  if (image.channels != 1 && image.channels != 3) {
    return fail<std::vector<uint8_t>>(Status::UnsupportedFormat,
                                      "jpeg: unsupported channel count");
  }

  jpeg_compress_struct cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));

  JpegError jerr;
  std::memset(&jerr, 0, sizeof(jerr));

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;
  jerr.pub.emit_message = jpegEmitMessage;

  unsigned char* buffer = nullptr;
  unsigned long buffer_size = 0;

  if (setjmp(jerr.jump) != 0) {
    jpeg_destroy_compress(&cinfo);
    if (buffer != nullptr) std::free(buffer);
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      std::string("jpeg: ") + jerr.message);
  }

  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &buffer, &buffer_size);

  cinfo.image_width = static_cast<JDIMENSION>(image.width);
  cinfo.image_height = static_cast<JDIMENSION>(image.height);
  cinfo.input_components = image.channels;
  cinfo.in_color_space = image.channels == 1 ? JCS_GRAYSCALE : JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo,
                   params.jpeg_quality < 1
                       ? 1
                       : (params.jpeg_quality > 100 ? 100 : params.jpeg_quality),
                   TRUE);

  if (image.channels == 3) {
    // 先 set_defaults 再改采样因子：set_defaults 会把它们重置成 2x2。
    applySubsampling(&cinfo, params.jpeg_subsampling);
  }

  jpeg_start_compress(&cinfo, TRUE);

  const size_t row_bytes = image.rowBytes();
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(
        image.pixels.data() + static_cast<size_t>(cinfo.next_scanline) * row_bytes);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  std::vector<uint8_t> out(buffer, buffer + buffer_size);
  std::free(buffer);
  return out;
}

Result<Image> decodeJpeg(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return fail<Image>(Status::InvalidArgument, "jpeg: empty input");
  }

  jpeg_decompress_struct cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));

  JpegError jerr;
  std::memset(&jerr, 0, sizeof(jerr));

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;
  jerr.pub.emit_message = jpegEmitMessage;

  if (setjmp(jerr.jump) != 0) {
    jpeg_destroy_decompress(&cinfo);
    return fail<Image>(Status::DecodeFailed,
                       std::string("jpeg: ") + jerr.message);
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));

  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return fail<Image>(Status::DecodeFailed, "jpeg: bad header");
  }

  // 通道保真：灰度源就输出 1 通道。CMYK/YCCK 不在支持范围内（本库面向
  // 相机与检测设备的 RGB/灰度图，转换矩阵各实现不一致反而更危险）。
  if (cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK) {
    jpeg_destroy_decompress(&cinfo);
    return fail<Image>(Status::UnsupportedFormat,
                       "jpeg: CMYK/YCCK is not supported");
  }

  const bool gray = cinfo.jpeg_color_space == JCS_GRAYSCALE;
  cinfo.out_color_space = gray ? JCS_GRAYSCALE : JCS_RGB;

  jpeg_start_decompress(&cinfo);

  Image out;
  out.width = static_cast<int>(cinfo.output_width);
  out.height = static_cast<int>(cinfo.output_height);
  out.channels = gray ? 1 : 3;
  out.pixels.resize(static_cast<size_t>(out.width) * out.height *
                    static_cast<size_t>(out.channels));

  const size_t row_bytes = out.rowBytes();
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW row = out.pixels.data() +
                   static_cast<size_t>(cinfo.output_scanline) * row_bytes;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return out;
}

Result<int> probeJpegChannels(const uint8_t* data, size_t size) {
  // SOI(FFD8) 之后是一串「标记 + 长度」段，分量数只在 SOF 里。
  // 不走 libjpeg：这里要的只是两个字节，而 libjpeg 的头解析会顺带校验
  // 量化表/霍夫曼表，对「只想问几通道」来说太重，也更容易在坏数据上 longjmp。
  if (data == nullptr || size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
    return fail<int>(Status::UnsupportedFormat, "jpeg: bad signature");
  }

  size_t pos = 2;
  while (pos + 4 <= size) {
    if (data[pos] != 0xFF) {
      ++pos;  // 容错：段间可能夹着填充字节
      continue;
    }
    const uint8_t marker = data[pos + 1];
    if (marker == 0xFF) {
      ++pos;
      continue;
    }
    // 无长度的独立标记：SOI、RSTn（只在熵编码数据里出现）。
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
      pos += 2;
      continue;
    }
    if (marker == 0xD9) break;  // EOI：没有 SOF 就是坏流

    const size_t seg_len = (static_cast<size_t>(data[pos + 2]) << 8) |
                           static_cast<size_t>(data[pos + 3]);
    if (seg_len < 2 || pos + 2 + seg_len > size) break;

    // SOF 的判据是 0xC0..0xCF，但要排除 C4(DHT)、C8(JPG)、CC(DAC) —— 它们
    // 恰好落在这个区间里，却不是 SOF，读出来的「分量数」是别的字段。
    const bool is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                        marker != 0xC8 && marker != 0xCC;
    if (is_sof) {
      // len(2) + precision(1) + height(2) + width(2) + ncomp(1)
      if (pos + 10 > size) break;
      return data[pos + 9] == 1 ? 1 : 3;
    }
    pos += 2 + seg_len;
  }

  return fail<int>(Status::UnsupportedFormat, "jpeg: no SOF marker");
}

}  // namespace cpu
}  // namespace offload
}  // namespace image_codec
