// SPDX-License-Identifier: MIT
//
// libvisualmemory — embedding preprocessing "pp1" (std-only; shared by the
// RKNN backend, the host tools and the tests so host and device use ONE
// implementation).
//
//   1. Resize the WHOLE image to 224x224 (no crop, aspect NOT preserved) with a
//      separable antialiased bilinear (triangle) filter in float32 — Pillow's
//      precompute_coeffs(BILINEAR) without its 8-bit rounding.
//   2. x = (v/255 - mean[c]) / std[c], RGB order, ImageNet mean/std.
//   3. Output layout NHWC float32 [224][224][3].
//
// The Python reference (scripts/reference_embedder.py) implements the same
// algorithm independently; tests diff the two.
#ifndef VISUALMEMORY_PREPROCESS_H
#define VISUALMEMORY_PREPROCESS_H

#include "visualmemory/types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace visualmemory {

constexpr int kPp1Size = 224;
constexpr size_t kPp1Floats = static_cast<size_t>(kPp1Size) * kPp1Size * 3;
constexpr const char* kPp1Id = "pp1";

class Pp1Preprocessor {
public:
    // `out` must hold kPp1Floats floats (NHWC). Not thread-safe (reuses scratch
    // buffers and cached filter taps); callers serialise. Returns false and sets
    // `err` for invalid input (null data, size < 2, bad stride, unsupported format).
    bool run(const ImageView& img, float* out, std::string& err);

private:
    struct Taps {                       // one axis worth of filter taps
        int in = 0;
        std::vector<int>   start, count;
        std::vector<float> w;           // out*maxcount weights
        int maxcount = 0;
    };
    static void build(int in, int out, Taps& t);
    Taps tx_, ty_;
    std::vector<float> tmp_;
};

} // namespace visualmemory

#endif // VISUALMEMORY_PREPROCESS_H
