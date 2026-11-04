// SPDX-License-Identifier: MIT
// Internal: bounded JPEG -> RGB24 decode for the embedding path (TurboJPEG stays
// confined to the .cpp).
#ifndef VISUALMEMORY_JPEG_DECODER_H
#define VISUALMEMORY_JPEG_DECODER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace visualmemory::detail {

struct DecodedImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;     // packed RGB24, tightly packed
};

// Decodes at most `max_pixels` pixels (guards memory). Returns false + err on failure.
bool decode_jpeg_rgb(const uint8_t* data, size_t size, DecodedImage& out, std::string& err,
                     size_t max_pixels = 16u * 1000 * 1000);

// Read a whole file, refusing anything larger than max_bytes.
bool read_file_bounded(const std::string& path, size_t max_bytes, std::vector<uint8_t>& out,
                       std::string& err);

} // namespace visualmemory::detail

#endif // VISUALMEMORY_JPEG_DECODER_H
