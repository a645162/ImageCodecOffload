#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

#include "image_codec_types.h"

namespace image_codec::offload::cpu {

Result<std::vector<uint8_t>> encodeJpeg2000(const Image &image,
                                            Jpeg2000Container container);
// Calls rowReader exactly once for each top-down row and only retains one
// full-width tile (at most 256 rows) while writing the JP2/J2K stream.
using Jpeg2000RowReader =
    std::function<Result<void>(int y, std::span<uint8_t> destination)>;
Result<void> encodeJpeg2000Rows(const std::filesystem::path &path, int width,
                                int height, int channels,
                                Jpeg2000Container container,
                                const Jpeg2000RowReader &rowReader);
Result<Image> decodeJpeg2000(const uint8_t *data, size_t size);

} // namespace image_codec::offload::cpu
