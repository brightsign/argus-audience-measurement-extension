// SPDX-License-Identifier: MIT
#include "jpeg_decoder.h"

#include <turbojpeg.h>

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

bool read_file_bounded(const std::string& path, size_t max_bytes, std::vector<uint8_t>& out,
                       std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
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
