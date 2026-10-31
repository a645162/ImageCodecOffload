#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "image_codec_types.h"

namespace image_codec::offload::cpu {

Result<std::vector<uint8_t>> encodeJpeg2000(const Image &image, Format format);
Result<Image> decodeJpeg2000(const uint8_t *data, size_t size);

} // namespace image_codec::offload::cpu
