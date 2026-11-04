// SPDX-License-Identifier: MIT
#include "jpeg_decoder.h"

#include <turbojpeg.h>

#include <algorithm>
#include <cstdio>

namespace visualmemory::detail {

bool decode_jpeg_rgb(const uint8_t* data, size_t size, DecodedImage& out, std::string& err,
                     size_t max_pixels) {
    if (!data || size < 4) { err = "empty jpeg"; return false; }
    tjhandle tj = tjInitDecompress();
    if (!tj) { err = "tjInitDecompress failed"; return false; }
    int w = 0, h = 0, subsamp = 0, cs = 0;
    if (tjDecompressHeader3(tj, data, static_cast<unsigned long>(size), &w, &h, &subsamp, &cs) != 0) {
        err = std::string("jpeg header: ") + tjGetErrorStr2(tj);
        tjDestroy(tj);
        return false;
    }
    if (w <= 0 || h <= 0 || static_cast<size_t>(w) * static_cast<size_t>(h) > max_pixels) {
        err = "jpeg dimensions out of bounds";
        tjDestroy(tj);
        return false;
    }
    out.width = w; out.height = h;
    out.rgb.resize(static_cast<size_t>(w) * h * 3);
    // Accurate (ISLOW) DCT, the libjpeg default also used by Pillow: keeps the decoded pixels — and thus
    // the pp1 tensor — identical to the host reference implementation.
    if (tjDecompress2(tj, data, static_cast<unsigned long>(size), out.rgb.data(), w, 0, h, TJPF_RGB,
                      0) != 0) {
        // Corrupt-but-decodable JPEGs raise a warning (rc -1 with non-fatal flag); treat any error as failure.
        err = std::string("jpeg decode: ") + tjGetErrorStr2(tj);
        tjDestroy(tj);
        return false;
    }
    tjDestroy(tj);
    return true;
}

bool make_thumbnail(const uint8_t* data, size_t size, int max_w, int quality, std::vector<uint8_t>& out,
                    std::string& err) {
    if (!data || size < 4) { err = "empty jpeg"; return false; }
    tjhandle tj = tjInitDecompress();
    if (!tj) { err = "tjInitDecompress failed"; return false; }
    int w = 0, h = 0, subsamp = 0, cs = 0;
    if (tjDecompressHeader3(tj, data, static_cast<unsigned long>(size), &w, &h, &subsamp, &cs) != 0 ||
        w <= 0 || h <= 0 || static_cast<size_t>(w) * static_cast<size_t>(h) > 16u * 1000 * 1000) {
        err = "jpeg header invalid or too large";
        tjDestroy(tj);
        return false;
    }
    // Largest DCT-domain reduction that still gives >= max_w (cheap: decodes fewer coefficients).
    int nf = 0;
    tjscalingfactor* sf = tjGetScalingFactors(&nf);
    int bw = w, bh = h;
    for (int i = 0; sf && i < nf; ++i) {
        const int sw = TJSCALED(w, sf[i]), sh = TJSCALED(h, sf[i]);
        if (sw >= max_w && sw * sh < bw * bh) { bw = sw; bh = sh; }
    }
    std::vector<uint8_t> px(static_cast<size_t>(bw) * bh * 3);
    if (tjDecompress2(tj, data, static_cast<unsigned long>(size), px.data(), bw, 0, bh, TJPF_RGB,
                      TJFLAG_FASTDCT) != 0) {
        err = "jpeg decode failed";
        tjDestroy(tj);
        return false;
    }
    tjDestroy(tj);
    // Box downscale to exactly max_w (never upscale).
    int ow = std::min(bw, max_w), oh = std::max(1, static_cast<int>(static_cast<int64_t>(bh) * ow / bw));
    std::vector<uint8_t> small;
    const uint8_t* src = px.data(); int sw2 = bw, sh2 = bh;
    if (ow != bw) {
        small.resize(static_cast<size_t>(ow) * oh * 3);
        for (int y = 0; y < oh; ++y) {
            const int y0 = static_cast<int>(static_cast<int64_t>(y) * bh / oh);
            const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * bh / oh));
            for (int x = 0; x < ow; ++x) {
                const int x0 = static_cast<int>(static_cast<int64_t>(x) * bw / ow);
                const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * bw / ow));
                unsigned acc[3] = {0, 0, 0}; unsigned n = 0;
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx) {
                        const uint8_t* p = px.data() + (static_cast<size_t>(yy) * bw + xx) * 3;
                        acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; ++n;
                    }
                uint8_t* d = small.data() + (static_cast<size_t>(y) * ow + x) * 3;
                d[0] = acc[0] / n; d[1] = acc[1] / n; d[2] = acc[2] / n;
            }
        }
        src = small.data(); sw2 = ow; sh2 = oh;
    }
    tjhandle tc = tjInitCompress();
    if (!tc) { err = "tjInitCompress failed"; return false; }
    unsigned char* buf = nullptr; unsigned long bsz = 0;
    const int rc = tjCompress2(tc, src, sw2, 0, sh2, TJPF_RGB, &buf, &bsz, TJSAMP_420, quality, TJFLAG_FASTDCT);
    if (rc != 0) { err = "jpeg encode failed"; tjDestroy(tc); if (buf) tjFree(buf); return false; }
    out.assign(buf, buf + bsz);
    tjFree(buf);
    tjDestroy(tc);
    return true;
}

bool read_file_bounded(const std::string& path, size_t max_bytes, std::vector<uint8_t>& out,
                       std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open file"; return false; }   // never echo filesystem paths
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0 || static_cast<size_t>(n) > max_bytes) {
        std::fclose(f);
        err = n <= 0 ? "empty file" : "file too large";
        return false;
    }
    out.resize(static_cast<size_t>(n));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) { err = "short read"; return false; }
    return true;
}

} // namespace visualmemory::detail
