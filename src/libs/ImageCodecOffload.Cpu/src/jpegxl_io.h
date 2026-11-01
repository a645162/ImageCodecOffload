#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>
#include "image_codec_types.h"
namespace image_codec::offload::cpu {
using JpegXlRowReader =
    std::function<Result<void>(int y, std::span<uint8_t> destination)>;
Result<std::vector<uint8_t>> encodeJpegXl(const Image &image);
Result<std::vector<uint8_t>> encodeJpegXlRows(int width, int height,
                                              int channels,
                                              const JpegXlRowReader &rowReader);
Result<Image> decodeJpegXl(const uint8_t *data, size_t size);
} // namespace image_codec::offload::cpu
