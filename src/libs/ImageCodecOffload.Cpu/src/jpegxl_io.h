#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "image_codec_types.h"
namespace image_codec::offload::cpu {
Result<std::vector<uint8_t>> encodeJpegXl(const Image &image);
Result<Image> decodeJpegXl(const uint8_t *data, size_t size);
} // namespace image_codec::offload::cpu
