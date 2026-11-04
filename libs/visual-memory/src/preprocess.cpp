// SPDX-License-Identifier: MIT
#include "visualmemory/preprocess.h"

#include <algorithm>
#include <cmath>

namespace visualmemory {

// Pillow's precompute_coeffs for the BILINEAR (triangle) filter, support 1.0.
void Pp1Preprocessor::build(int in, int out, Taps& t) {
    t.in = in;
    const double scale = static_cast<double>(in) / out;
    const double filterscale = std::max(scale, 1.0);
    const double support = 1.0 * filterscale;
    t.maxcount = static_cast<int>(std::ceil(support)) * 2 + 1;
    t.start.assign(out, 0);
    t.count.assign(out, 0);
    t.w.assign(static_cast<size_t>(out) * t.maxcount, 0.0f);
    for (int i = 0; i < out; ++i) {
        const double center = (i + 0.5) * scale;
        int xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = static_cast<int>(center + support + 0.5);
        if (xmax > in) xmax = in;
        const int n = xmax - xmin;
        double sum = 0;
        std::vector<double> ww(n > 0 ? n : 0);
        for (int j = 0; j < n; ++j) {
            const double x = (j + xmin - center + 0.5) / filterscale;
            const double v = std::max(0.0, 1.0 - std::fabs(x));
            ww[j] = v;
            sum += v;
        }
        t.start[i] = xmin;
        t.count[i] = n;
        if (sum != 0.0)
            for (int j = 0; j < n; ++j)
                t.w[static_cast<size_t>(i) * t.maxcount + j] = static_cast<float>(ww[j] / sum);
    }
}

bool Pp1Preprocessor::run(const ImageView& img, float* out, std::string& err) {
    if (!img.data || !out) { err = "null pointer"; return false; }
    if (img.format != PixelFormat::RGB24 && img.format != PixelFormat::BGR24) {
        err = "unsupported pixel format (need RGB24/BGR24)"; return false;
    }
    if (img.width < 2 || img.height < 2 || img.width > 16384 || img.height > 16384) {
        err = "invalid image dimensions"; return false;
    }
    const int stride = img.stride ? img.stride : img.width * 3;
    if (stride < img.width * 3) { err = "stride smaller than row size"; return false; }

    if (tx_.in != img.width || tx_.start.size() != static_cast<size_t>(kPp1Size)) build(img.width, kPp1Size, tx_);
    if (ty_.in != img.height || ty_.start.size() != static_cast<size_t>(kPp1Size)) build(img.height, kPp1Size, ty_);

    const int W = kPp1Size;
    tmp_.resize(static_cast<size_t>(img.height) * W * 3);
    const bool bgr = img.format == PixelFormat::BGR24;
    const int ro = bgr ? 2 : 0, bo = bgr ? 0 : 2;   // source offsets of R and B

    // Horizontal pass: [in_h][in_w][3] u8 -> [in_h][224][3] float (RGB order).
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.data + static_cast<size_t>(y) * stride;
        float* trow = tmp_.data() + static_cast<size_t>(y) * W * 3;
        for (int x = 0; x < W; ++x) {
            const int s = tx_.start[x], n = tx_.count[x];
            const float* w = &tx_.w[static_cast<size_t>(x) * tx_.maxcount];
            float r = 0, g = 0, b = 0;
            for (int j = 0; j < n; ++j) {
                const uint8_t* p = row + static_cast<size_t>(s + j) * 3;
                r += w[j] * p[ro]; g += w[j] * p[1]; b += w[j] * p[bo];
            }
            trow[x * 3 + 0] = r; trow[x * 3 + 1] = g; trow[x * 3 + 2] = b;
        }
    }
    // Vertical pass + normalisation -> NHWC float.
    static const float kMean[3] = {0.485f, 0.456f, 0.406f};
    static const float kStd[3]  = {0.229f, 0.224f, 0.225f};
    for (int y = 0; y < W; ++y) {
        const int s = ty_.start[y], n = ty_.count[y];
        const float* w = &ty_.w[static_cast<size_t>(y) * ty_.maxcount];
        float* orow = out + static_cast<size_t>(y) * W * 3;
        for (int x = 0; x < W * 3; ++x) {
            float acc = 0;
            for (int j = 0; j < n; ++j) acc += w[j] * tmp_[static_cast<size_t>(s + j) * W * 3 + x];
            const int c = x % 3;
            orow[x] = (acc / 255.0f - kMean[c]) / kStd[c];
        }
    }
    return true;
}

} // namespace visualmemory
