#include "openjpeg_io.h"

#ifdef IMAGECODEC_OFFLOAD_WITH_OPENJPEG
#include <openjpeg.h>
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>

namespace image_codec::offload::cpu {
#ifdef IMAGECODEC_OFFLOAD_WITH_OPENJPEG
namespace {

struct MemoryStream {
  const uint8_t *input = nullptr;
  size_t inputSize = 0;
  size_t inputOffset = 0;
  std::vector<uint8_t> output;
  size_t outputOffset = 0;
  bool writing = false;
};

OPJ_SIZE_T readCallback(void *buffer, OPJ_SIZE_T size, void *user) {
  auto &stream = *static_cast<MemoryStream *>(user);
  const size_t remaining = stream.inputSize - stream.inputOffset;
  const size_t count = std::min<size_t>(remaining, size);
  if (count != 0) std::memcpy(buffer, stream.input + stream.inputOffset, count);
  stream.inputOffset += count;
  return static_cast<OPJ_SIZE_T>(count);
}

OPJ_SIZE_T writeCallback(void *buffer, OPJ_SIZE_T size, void *user) {
  auto &stream = *static_cast<MemoryStream *>(user);
  const auto *bytes = static_cast<const uint8_t *>(buffer);
  if (stream.outputOffset + size > stream.output.size())
    stream.output.resize(stream.outputOffset + size);
  std::memcpy(stream.output.data() + stream.outputOffset, bytes, size);
  stream.outputOffset += size;
  return size;
}

OPJ_OFF_T skipCallback(OPJ_OFF_T count, void *user) {
  auto &stream = *static_cast<MemoryStream *>(user);
  if (stream.writing) {
    if (count < 0) {
      const size_t delta = static_cast<size_t>(-count);
      if (delta > stream.outputOffset) return -1;
      stream.outputOffset -= delta;
    } else {
      stream.outputOffset += static_cast<size_t>(count);
    }
    if (stream.outputOffset > stream.output.size())
      stream.output.resize(stream.outputOffset);
    return count;
  }
  if (count < 0) {
    const size_t delta = static_cast<size_t>(-count);
    if (delta > stream.inputOffset) return -1;
    stream.inputOffset -= delta;
    return count;
  }
  const size_t target = stream.inputOffset + static_cast<size_t>(count);
  if (target > stream.inputSize) return -1;
  stream.inputOffset = target;
  return count;
}

OPJ_BOOL seekCallback(OPJ_OFF_T position, void *user) {
  auto &stream = *static_cast<MemoryStream *>(user);
  if (position < 0) return OPJ_FALSE;
  if (stream.writing) {
    stream.outputOffset = static_cast<size_t>(position);
    if (stream.outputOffset > stream.output.size())
      stream.output.resize(stream.outputOffset);
  } else {
    if (static_cast<size_t>(position) > stream.inputSize) return OPJ_FALSE;
    stream.inputOffset = static_cast<size_t>(position);
  }
  return OPJ_TRUE;
}


opj_stream_t *makeStream(MemoryStream &memory, bool output) {
  memory.writing = output;
  auto *stream = opj_stream_default_create(output ? OPJ_FALSE : OPJ_TRUE);
  if (stream == nullptr) return nullptr;
  opj_stream_set_user_data(stream, &memory, nullptr);
  if (!output) {
    opj_stream_set_user_data_length(
        stream, static_cast<OPJ_UINT64>(memory.inputSize));
  }
  opj_stream_set_read_function(stream, readCallback);
  opj_stream_set_write_function(stream, writeCallback);
  opj_stream_set_skip_function(stream, skipCallback);
  opj_stream_set_seek_function(stream, seekCallback);
  return stream;
}

opj_codec_t *makeCodec(Format format, bool encode) {
  if (format == Format::Jpeg2000) {
    return encode ? opj_create_compress(OPJ_CODEC_JP2)
                  : opj_create_decompress(OPJ_CODEC_JP2);
  }
  return encode ? opj_create_compress(OPJ_CODEC_J2K)
                : opj_create_decompress(OPJ_CODEC_J2K);
}

} // namespace

Result<std::vector<uint8_t>> encodeJpeg2000(const Image &image, Format format) {
  if (!image.valid() || (format != Format::Jpeg2000 && format != Format::JpegXl))
    return fail<std::vector<uint8_t>>(Status::InvalidArgument,
                                      "openjpeg: invalid image or format");
  if (image.channels != 1 && image.channels != 3)
    return fail<std::vector<uint8_t>>(Status::UnsupportedFormat,
                                      "openjpeg: only gray and RGB are supported");
  if (format == Format::JpegXl)
    return fail<std::vector<uint8_t>>(Status::UnsupportedFormat,
                                      "jpeg-xl requires libjxl");

  opj_cparameters_t parameters;
  opj_set_default_encoder_parameters(&parameters);
  parameters.tcp_numlayers = 1;
  parameters.cp_disto_alloc = 1;
  parameters.numresolution = 1;
  for (int dimension = std::min(image.width, image.height);
       dimension > 1 && parameters.numresolution < 6; dimension >>= 1) {
    ++parameters.numresolution;
  }

  opj_image_cmptparm_t components[3]{};
  for (int channel = 0; channel < image.channels; ++channel) {
    components[channel].dx = 1;
    components[channel].dy = 1;
    components[channel].w = static_cast<OPJ_UINT32>(image.width);
    components[channel].h = static_cast<OPJ_UINT32>(image.height);
    components[channel].prec = 8;
    components[channel].sgnd = 0;
  }
  opj_image_t *raw = opj_image_create(
      static_cast<OPJ_UINT32>(image.channels), components,
      image.channels == 1 ? OPJ_CLRSPC_GRAY : OPJ_CLRSPC_SRGB);
  if (raw == nullptr)
    return fail<std::vector<uint8_t>>(Status::OutOfMemory,
                                      "openjpeg: image allocation failed");
  raw->x0 = 0;
  raw->y0 = 0;
  raw->x1 = static_cast<OPJ_UINT32>(image.width);
  raw->y1 = static_cast<OPJ_UINT32>(image.height);
  for (int channel = 0; channel < image.channels; ++channel) {
    auto &component = raw->comps[channel];
    for (size_t pixel = 0; pixel < image.pixels.size() / image.channels;
         ++pixel)
      component.data[pixel] = image.pixels[pixel * image.channels + channel];
  }

  const auto path = std::filesystem::temp_directory_path() /
                    ("imagecodec-jp2-" +
                     std::to_string(std::chrono::high_resolution_clock::now()
                                        .time_since_epoch()
                                        .count()) +
                     ".jp2");
  opj_codec_t *codec = makeCodec(Format::Jpeg2000, true);
  opj_stream_t *stream = codec == nullptr
                             ? nullptr
                             : opj_stream_create_default_file_stream(
                                   path.string().c_str(), OPJ_FALSE);
  bool ok = codec != nullptr && stream != nullptr;
  if (ok) ok = opj_setup_encoder(codec, &parameters, raw) == OPJ_TRUE;
  if (ok) ok = opj_start_compress(codec, raw, stream) == OPJ_TRUE;
  if (ok) ok = opj_encode(codec, stream) == OPJ_TRUE;
  if (ok) ok = opj_end_compress(codec, stream) == OPJ_TRUE;
  if (stream != nullptr) opj_stream_destroy(stream);
  if (codec != nullptr) opj_destroy_codec(codec);
  opj_image_destroy(raw);
  if (!ok)
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "openjpeg: encode failed");
  std::ifstream file(path, std::ios::binary);
  std::vector<uint8_t> output((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  if (output.empty())
    return fail<std::vector<uint8_t>>(Status::EncodeFailed,
                                      "openjpeg: empty output");
  return output;
}

Result<Image> decodeJpeg2000(const uint8_t *data, size_t size) {
  if (data == nullptr || size == 0)
    return fail<Image>(Status::InvalidArgument, "openjpeg: empty input");
  const auto path = std::filesystem::temp_directory_path() /
                    ("imagecodec-jp2-decode-" +
                     std::to_string(std::chrono::high_resolution_clock::now()
                                        .time_since_epoch()
                                        .count()) +
                     ".jp2");
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(data),
               static_cast<std::streamsize>(size));
  }
  const bool rawCodestream = size >= 4 && data[0] == 0xFF && data[1] == 0x4F &&
                             data[2] == 0xFF && data[3] == 0x51;
  opj_codec_t *codec = opj_create_decompress(rawCodestream ? OPJ_CODEC_J2K
                                                            : OPJ_CODEC_JP2);
  opj_dparameters_t parameters;
  opj_set_default_decoder_parameters(&parameters);
  opj_stream_t *stream = codec == nullptr
                             ? nullptr
                             : opj_stream_create_default_file_stream(
                                   path.string().c_str(), OPJ_TRUE);
  opj_image_t *raw = nullptr;
  bool ok = codec != nullptr && stream != nullptr;
  if (ok) ok = opj_setup_decoder(codec, &parameters) == OPJ_TRUE;
  if (ok) ok = opj_read_header(stream, codec, &raw) == OPJ_TRUE;
  if (ok) ok = opj_decode(codec, stream, raw) == OPJ_TRUE;
  if (ok) ok = opj_end_decompress(codec, stream) == OPJ_TRUE;
  if (stream != nullptr) opj_stream_destroy(stream);
  if (codec != nullptr) opj_destroy_codec(codec);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  if (!ok || raw == nullptr)
    return fail<Image>(Status::DecodeFailed, "openjpeg: decode failed");
  if (raw->numcomps != 1 && raw->numcomps != 3) {
    opj_image_destroy(raw);
    return fail<Image>(Status::UnsupportedFormat,
                       "openjpeg: output must be gray or RGB");
  }
  Image image;
  image.width = static_cast<int>(raw->comps[0].w);
  image.height = static_cast<int>(raw->comps[0].h);
  image.channels = static_cast<int>(raw->numcomps);
  image.pixels.resize(static_cast<size_t>(image.width) * image.height *
                      image.channels);
  for (size_t pixel = 0; pixel < static_cast<size_t>(image.width) * image.height;
       ++pixel) {
    for (int channel = 0; channel < image.channels; ++channel) {
      const int value = raw->comps[channel].data[pixel];
      image.pixels[pixel * image.channels + channel] =
          static_cast<uint8_t>(std::clamp(value, 0, 255));
    }
  }
  opj_image_destroy(raw);
  return image;
}

} // namespace image_codec::offload::cpu
#else

} // namespace image_codec::offload::cpu

namespace image_codec::offload::cpu {
Result<std::vector<uint8_t>> encodeJpeg2000(const Image &, Format) {
  return fail<std::vector<uint8_t>>(Status::BackendUnavailable,
                                    "openjpeg is not built");
}
Result<Image> decodeJpeg2000(const uint8_t *, size_t) {
  return fail<Image>(Status::BackendUnavailable, "openjpeg is not built");
}
} // namespace image_codec::offload::cpu
#endif
