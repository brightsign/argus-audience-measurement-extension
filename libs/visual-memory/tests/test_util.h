// SPDX-License-Identifier: MIT
// Shared helpers for libvisualmemory host tests.
#ifndef VISUALMEMORY_TEST_UTIL_H
#define VISUALMEMORY_TEST_UTIL_H

#include "visualmemory/types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>

namespace vmtest {

// A unique temporary directory that is removed on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<uint64_t> counter{0};
        auto base = std::filesystem::temp_directory_path();
        auto pid = static_cast<uint64_t>(::getpid());
        auto n = counter.fetch_add(1);
        path_ = base / ("vm_test_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
    std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// Build a synthetic packed BGR24 frame with a deterministic gradient.
inline visualmemory::OwnedFrame make_bgr_frame(int w, int h, const std::string& source,
                                               int64_t capture_utc_ms, uint64_t seq) {
    visualmemory::OwnedFrame f;
    f.width = w;
    f.height = h;
    f.pixel_format = visualmemory::PixelFormat::BGR24;
    f.stride0 = w * 3;
    f.pixels.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = f.pixels.data() + (static_cast<size_t>(y) * w + x) * 3;
            p[0] = static_cast<uint8_t>(x % 256);          // B
            p[1] = static_cast<uint8_t>(y % 256);          // G
            p[2] = static_cast<uint8_t>((x + y) % 256);    // R
        }
    }
    f.metadata.source_id = source;
    f.metadata.source_type = "file";
    f.metadata.device_id = "test-device";
    f.metadata.capture_utc_ms = capture_utc_ms;
    f.metadata.source_sequence = seq;
    return f;
}

// Deterministic "scene": a 4x4 grid of random-colour blocks seeded by `seed`, optionally brightened
// by `shift` (0 => identical image). Distinct seeds give clearly different thumbnails; the same seed
// with a small shift gives a near-duplicate. Packed RGB24 (use as ImageView / as an OwnedFrame).
inline std::vector<uint8_t> make_scene_rgb(int w, int h, uint32_t seed, int shift = 0) {
    std::mt19937 g(seed);
    uint8_t col[16][3];
    for (auto& c : col) for (auto& v : c) v = static_cast<uint8_t>(g() % 256);
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int b = (y * 4 / h) * 4 + (x * 4 / w);
            for (int k = 0; k < 3; ++k) {
                int v = col[b][k] + shift;
                px[(static_cast<size_t>(y) * w + x) * 3 + k] = static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
    return px;
}

inline visualmemory::OwnedFrame make_scene_frame(int w, int h, uint32_t seed, const std::string& source,
                                                 int64_t capture_utc_ms, uint64_t seq, int shift = 0) {
    visualmemory::OwnedFrame f;
    f.width = w; f.height = h;
    f.pixel_format = visualmemory::PixelFormat::RGB24;
    f.stride0 = w * 3;
    f.pixels = make_scene_rgb(w, h, seed, shift);
    f.metadata.source_id = source; f.metadata.source_type = "file"; f.metadata.device_id = "test-device";
    f.metadata.capture_utc_ms = capture_utc_ms; f.metadata.source_sequence = seq;
    return f;
}

// Poll a predicate until true or timeout. Returns the final predicate value.
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

} // namespace vmtest

#endif // VISUALMEMORY_TEST_UTIL_H
