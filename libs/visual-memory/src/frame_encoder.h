// SPDX-License-Identifier: MIT
// Internal: frame validation, aspect-preserving resize and JPEG encoding.
// TurboJPEG stays confined to this translation unit; it must not leak into the
// public API.
#ifndef VISUALMEMORY_FRAME_ENCODER_H
#define VISUALMEMORY_FRAME_ENCODER_H

#include "visualmemory/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace visualmemory::detail {

struct EncodedJpeg {
    std::vector<uint8_t> bytes;
    int width  = 0;   // actual encoded width  (<= bound)
    int height = 0;   // actual encoded height (<= bound)
};

// Validate dimensions, strides and buffer length for the declared format.
bool validate_frame(const OwnedFrame& f, std::string& err);

// Encode `f` to JPEG, scaled to fit inside bound_w x bound_h while preserving
// aspect ratio (never upscaled, never stretched). `quality` is 1..100.
bool encode_frame(const OwnedFrame& f,
                  int bound_w, int bound_h, int quality,
                  EncodedJpeg& out, std::string& err);

} // namespace visualmemory::detail

#endif // VISUALMEMORY_FRAME_ENCODER_H
