// SPDX-License-Identifier: MIT
#include "frame_encoder.h"

#include <turbojpeg.h>

#include <algorithm>
#include <cstring>

namespace visualmemory::detail {
namespace {

constexpr int kMaxDim = 20000;  // sanity bound against overflow / bogus input

// Expected tightly-packed stride for plane 0 of each format.
int packed_stride0(const OwnedFrame& f) {
    switch (f.pixel_format) {
        case PixelFormat::RGB24:
        case PixelFormat::BGR24: return f.width * 3;
        case PixelFormat::NV12:  return f.width;      // Y plane
        case PixelFormat::JPEG:  return 0;
    }
    return 0;
}

// Clamp helper for colour conversion.
inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Convert NV12 -> packed RGB24 (BT.601 full-ish). Output tightly packed.
void nv12_to_rgb(const OwnedFrame& f, std::vector<uint8_t>& rgb) {
    const int w = f.width, h = f.height;
    const int sy = f.stride0 > 0 ? f.stride0 : w;
    const int suv = f.stride1 > 0 ? f.stride1 : w;
    const uint8_t* Y = f.pixels.data();
    const uint8_t* UV = f.pixels.data() + static_cast<size_t>(sy) * h;
    rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        const uint8_t* yrow = Y + static_cast<size_t>(y) * sy;
        const uint8_t* uvrow = UV + static_cast<size_t>(y / 2) * suv;
        uint8_t* out = rgb.data() + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; ++x) {
            int C = yrow[x] - 16;
            int u = uvrow[(x / 2) * 2 + 0] - 128;
            int v = uvrow[(x / 2) * 2 + 1] - 128;
            if (C < 0) C = 0;
            out[x * 3 + 0] = clamp8((298 * C + 409 * v + 128) >> 8);          // R
            out[x * 3 + 1] = clamp8((298 * C - 100 * u - 208 * v + 128) >> 8); // G
            out[x * 3 + 2] = clamp8((298 * C + 516 * u + 128) >> 8);          // B
        }
    }
}

// Bilinear resize of a packed 3-channel image. Channel order is preserved.
void resize_rgb3(const uint8_t* src, int sw, int sh, int src_stride,
                 uint8_t* dst, int dw, int dh) {
    const int dst_stride = dw * 3;
    const float fx = (dw > 1) ? static_cast<float>(sw - 1) / (dw - 1) : 0.f;
    const float fy = (dh > 1) ? static_cast<float>(sh - 1) / (dh - 1) : 0.f;
    for (int y = 0; y < dh; ++y) {
        float syf = y * fy;
        int y0 = static_cast<int>(syf);
        int y1 = std::min(y0 + 1, sh - 1);
        float wy = syf - y0;
        const uint8_t* r0 = src + static_cast<size_t>(y0) * src_stride;
        const uint8_t* r1 = src + static_cast<size_t>(y1) * src_stride;
        uint8_t* orow = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < dw; ++x) {
            float sxf = x * fx;
            int x0 = static_cast<int>(sxf);
            int x1 = std::min(x0 + 1, sw - 1);
            float wx = sxf - x0;
            const uint8_t* p00 = r0 + x0 * 3;
            const uint8_t* p01 = r0 + x1 * 3;
            const uint8_t* p10 = r1 + x0 * 3;
            const uint8_t* p11 = r1 + x1 * 3;
            for (int c = 0; c < 3; ++c) {
                float top = p00[c] * (1 - wx) + p01[c] * wx;
                float bot = p10[c] * (1 - wx) + p11[c] * wx;
                orow[x * 3 + c] = static_cast<uint8_t>(top * (1 - wy) + bot * wy + 0.5f);
            }
        }
    }
}

} // namespace

bool validate_frame(const OwnedFrame& f, std::string& err) {
    if (f.pixel_format == PixelFormat::JPEG) {
        if (f.pixels.empty()) { err = "empty JPEG payload"; return false; }
        if (f.width <= 0 || f.height <= 0 || f.width > kMaxDim || f.height > kMaxDim) {
            err = "invalid JPEG dimensions"; return false;
        }
        return true;
    }
    if (f.width <= 0 || f.height <= 0 || f.width > kMaxDim || f.height > kMaxDim) {
        err = "invalid dimensions"; return false;
    }
    const int s0 = f.stride0 > 0 ? f.stride0 : packed_stride0(f);
    if (s0 < packed_stride0(f)) { err = "stride0 smaller than row width"; return false; }

    size_t need = 0;
    if (f.pixel_format == PixelFormat::RGB24 || f.pixel_format == PixelFormat::BGR24) {
        need = static_cast<size_t>(s0) * f.height;
    } else { // NV12
        const int s1 = f.stride1 > 0 ? f.stride1 : f.width;
        if (s1 < f.width) { err = "stride1 smaller than UV row width"; return false; }
        need = static_cast<size_t>(s0) * f.height +
               static_cast<size_t>(s1) * ((f.height + 1) / 2);
    }
    if (f.pixels.size() < need) {
        err = "pixel buffer too small for declared geometry"; return false;
    }
    return true;
}

bool encode_frame(const OwnedFrame& f, int bound_w, int bound_h, int quality,
                  EncodedJpeg& out, std::string& err) {
    if (!validate_frame(f, err)) return false;

    if (f.pixel_format == PixelFormat::JPEG) {
        out.bytes = f.pixels;           // store verbatim
        out.width = f.width;
        out.height = f.height;
        return true;
    }
    if (bound_w <= 0 || bound_h <= 0) { err = "invalid bound box"; return false; }
    quality = std::clamp(quality, 1, 100);

    // Obtain a packed 3-channel source (and remember its TurboJPEG pixel order).
    const uint8_t* src = nullptr;
    int src_stride = 0;
    int tjpf = TJPF_RGB;
    std::vector<uint8_t> converted;  // backing store for NV12 conversion

    if (f.pixel_format == PixelFormat::BGR24) {
        src = f.pixels.data();
        src_stride = f.stride0 > 0 ? f.stride0 : f.width * 3;
        tjpf = TJPF_BGR;
    } else if (f.pixel_format == PixelFormat::RGB24) {
        src = f.pixels.data();
        src_stride = f.stride0 > 0 ? f.stride0 : f.width * 3;
        tjpf = TJPF_RGB;
    } else { // NV12
        nv12_to_rgb(f, converted);
        src = converted.data();
        src_stride = f.width * 3;
        tjpf = TJPF_RGB;
    }

    // Aspect-preserving fit (never upscale).
    const float scale = std::min({static_cast<float>(bound_w) / f.width,
                                  static_cast<float>(bound_h) / f.height, 1.0f});
    int dw = std::max(1, static_cast<int>(f.width * scale));
    int dh = std::max(1, static_cast<int>(f.height * scale));

    std::vector<uint8_t> resized;
    const uint8_t* enc_src = src;
    int enc_stride = src_stride;
    if (dw != f.width || dh != f.height) {
        resized.resize(static_cast<size_t>(dw) * dh * 3);
        resize_rgb3(src, f.width, f.height, src_stride, resized.data(), dw, dh);
        enc_src = resized.data();
        enc_stride = dw * 3;
    }

    tjhandle tj = tjInitCompress();
    if (!tj) { err = "tjInitCompress failed"; return false; }
    unsigned char* jpeg = nullptr;
    unsigned long jpeg_size = 0;
    int rc = tjCompress2(tj, enc_src, dw, enc_stride, dh, tjpf,
                         &jpeg, &jpeg_size, TJSAMP_420, quality, TJFLAG_FASTDCT);
    if (rc != 0) {
        err = std::string("tjCompress2 failed: ") + tjGetErrorStr2(tj);
        if (jpeg) tjFree(jpeg);
        tjDestroy(tj);
        return false;
    }
    out.bytes.assign(jpeg, jpeg + jpeg_size);
    out.width = dw;
    out.height = dh;
    tjFree(jpeg);
    tjDestroy(tj);
    return true;
}

} // namespace visualmemory::detail
