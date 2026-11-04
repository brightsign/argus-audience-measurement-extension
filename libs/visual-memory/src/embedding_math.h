// SPDX-License-Identifier: MIT
// Internal: validation + L2 normalisation of embedding vectors.
#ifndef VISUALMEMORY_EMBEDDING_MATH_H
#define VISUALMEMORY_EMBEDDING_MATH_H

#include <cmath>
#include <cstddef>
#include <vector>

namespace visualmemory::detail {

enum class VecCheck { Ok, BadDimension, NonFinite, ZeroNorm };

// Flatten-checks `v`, rejects wrong dimension / NaN / Inf / near-zero norm, then
// L2-normalises in place (double accumulation). On failure `v` is left unchanged.
inline VecCheck l2_normalize(std::vector<float>& v, int expected_dim, double min_norm = 1e-6) {
    if (expected_dim > 0 && v.size() != static_cast<size_t>(expected_dim)) return VecCheck::BadDimension;
    if (v.empty()) return VecCheck::BadDimension;
    double s = 0;
    for (float x : v) {
        if (!std::isfinite(x)) return VecCheck::NonFinite;
        s += static_cast<double>(x) * x;
    }
    const double n = std::sqrt(s);
    if (!(n > min_norm) || !std::isfinite(n)) return VecCheck::ZeroNorm;
    const double inv = 1.0 / n;
    for (float& x : v) x = static_cast<float>(x * inv);
    return VecCheck::Ok;
}

inline const char* to_string(VecCheck c) {
    switch (c) {
        case VecCheck::Ok: return "ok";
        case VecCheck::BadDimension: return "wrong dimension";
        case VecCheck::NonFinite: return "non-finite value";
        case VecCheck::ZeroNorm: return "near-zero norm";
    }
    return "?";
}

} // namespace visualmemory::detail

#endif // VISUALMEMORY_EMBEDDING_MATH_H
