// SPDX-License-Identifier: MIT
#include "visualmemory/mock_embedding_provider.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace visualmemory {

namespace {
// splitmix64 -> deterministic projection coefficients in [-1,1]
float coeff(uint64_t i, uint64_t j) {
    uint64_t z = (i * 0x9E3779B97F4A7C15ULL) ^ (j + 0xD1B54A32D192ED03ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<float>(static_cast<double>(z >> 11) / 9007199254740992.0 * 2.0 - 1.0);
}
} // namespace

MockEmbeddingProvider::MockEmbeddingProvider(int dimension, std::string version)
    : dim_(dimension), version_(std::move(version)) {}

EmbeddingSpec MockEmbeddingProvider::spec() const {
    EmbeddingSpec s;
    s.model_name = "mock_embedder";
    s.model_version = version_;
    s.dimension = dim_;
    s.preprocessing = "mock-thumb8x8";
    s.model_sha256 = "";
    return s;
}

std::string MockEmbeddingProvider::last_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return err_;
}

EmbeddingProviderStats MockEmbeddingProvider::provider_stats() const { return {}; }

bool MockEmbeddingProvider::embed(const ImageView& im, std::vector<float>& out) {
    calls_.fetch_add(1);
    auto fail = [&](const char* m) { std::lock_guard<std::mutex> lk(mu_); err_ = m; return false; };
    if (!ready_.load()) return fail("mock provider not ready");
    if (delay_ms_.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_.load()));
    if (fail_next_.load() > 0) { fail_next_.fetch_sub(1); return fail("injected failure"); }
    if (!im.data || im.width < 1 || im.height < 1) return fail("invalid image");
    if (im.format != PixelFormat::RGB24 && im.format != PixelFormat::BGR24) return fail("unsupported format");
    const int stride = im.stride ? im.stride : im.width * 3;

    // 8x8 area-averaged thumbnail, RGB order (BGR swapped), 192 values in [0,1].
    constexpr int T = 8;
    float thumb[T * T * 3] = {0};
    int cnt[T * T] = {0};
    for (int y = 0; y < im.height; ++y) {
        const int ty = y * T / im.height;
        const uint8_t* row = im.data + static_cast<size_t>(y) * stride;
        for (int x = 0; x < im.width; ++x) {
            const int tx = x * T / im.width;
            const uint8_t* p = row + static_cast<size_t>(x) * 3;
            const int c = ty * T + tx;
            thumb[c * 3 + 0] += (im.format == PixelFormat::RGB24 ? p[0] : p[2]);
            thumb[c * 3 + 1] += p[1];
            thumb[c * 3 + 2] += (im.format == PixelFormat::RGB24 ? p[2] : p[0]);
            ++cnt[c];
        }
    }
    for (int c = 0; c < T * T; ++c)
        for (int k = 0; k < 3; ++k) thumb[c * 3 + k] = cnt[c] ? thumb[c * 3 + k] / cnt[c] / 255.0f - 0.5f : 0.0f;

    out.assign(static_cast<size_t>(dim_), 0.0f);
    if (zero_next_.load() > 0) { zero_next_.fetch_sub(1); return true; }
    for (int d = 0; d < dim_; ++d) {
        float acc = 0;
        for (int i = 0; i < T * T * 3; ++i) acc += coeff(static_cast<uint64_t>(d), static_cast<uint64_t>(i)) * thumb[i];
        out[static_cast<size_t>(d)] = acc;
    }
    if (nan_next_.load() > 0) { nan_next_.fetch_sub(1); out[0] = std::numeric_limits<float>::quiet_NaN(); }
    return true;
}

} // namespace visualmemory
