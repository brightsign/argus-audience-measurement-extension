// SPDX-License-Identifier: MIT
//
// libvisualmemory — deterministic mock embedding provider for tests and demos
// on hosts without an NPU. Embeds an image as a fixed pseudo-random projection
// of its 8x8 area-averaged RGB thumbnail: identical images give identical
// vectors, similar images give nearby vectors, different images differ.
// Supports failure injection. NOT a quality model.
#ifndef VISUALMEMORY_MOCK_EMBEDDING_PROVIDER_H
#define VISUALMEMORY_MOCK_EMBEDDING_PROVIDER_H

#include "visualmemory/embedding_provider.h"

#include <atomic>
#include <mutex>

namespace visualmemory {

class MockEmbeddingProvider : public EmbeddingProvider {
public:
    explicit MockEmbeddingProvider(int dimension = 64, std::string version = "mock-1");

    EmbeddingSpec spec() const override;
    bool embed(const ImageView& image, std::vector<float>& out) override;
    bool ready() const override { return ready_.load(); }
    std::string last_error() const override;
    EmbeddingProviderStats provider_stats() const override;
    void shutdown() override { ready_.store(false); }

    // ---- test controls ----
    void set_ready(bool r) { ready_.store(r); }
    void fail_next(int n) { fail_next_.store(n); }          // next n calls return false
    void nan_next(int n)  { nan_next_.store(n); }           // next n calls output NaNs
    void zero_next(int n) { zero_next_.store(n); }          // next n calls output zero vectors
    void set_delay_ms(int ms) { delay_ms_.store(ms); }
    uint64_t calls() const { return calls_.load(); }

private:
    int dim_; std::string version_;
    std::atomic<bool> ready_{true};
    std::atomic<int> fail_next_{0}, nan_next_{0}, zero_next_{0}, delay_ms_{0};
    std::atomic<uint64_t> calls_{0};
    mutable std::mutex mu_;
    std::string err_;
};

} // namespace visualmemory

#endif // VISUALMEMORY_MOCK_EMBEDDING_PROVIDER_H
