// SPDX-License-Identifier: MIT
//
// libvisualmemory — fixed-size latency recorder (header-only, std only).
// Shared by the core library and optional backends (e.g. the RKNN provider).
#ifndef VISUALMEMORY_LATENCY_RING_H
#define VISUALMEMORY_LATENCY_RING_H

#include "visualmemory/types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace visualmemory {

inline int64_t steady_now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// Ring of up to 1024 latencies (microseconds). add() is a few ns of an
// uncontended mutex; snapshot() sorts a copy (nearest-rank percentiles).
class LatencyRing {
public:
    void add(int64_t ns) {
        if (ns < 0) ns = 0;
        const uint32_t us = static_cast<uint32_t>(std::min<int64_t>(ns / 1000, 0xFFFFFFFFLL));
        std::lock_guard<std::mutex> lk(mu_);
        if (buf_.size() < kCap) buf_.push_back(us); else buf_[next_] = us;
        next_ = (next_ + 1) % kCap;
        ++count_;
        if (us > max_us_) max_us_ = us;
    }
    LatencyStats snapshot() const {
        std::vector<uint32_t> v;
        LatencyStats s;
        {
            std::lock_guard<std::mutex> lk(mu_);
            v = buf_;
            s.count = count_;
            s.max_ms = max_us_ / 1000.0;
        }
        if (v.empty()) return s;
        std::sort(v.begin(), v.end());
        auto pct = [&](double p) {
            size_t idx = static_cast<size_t>(std::ceil(p * v.size()));
            if (idx > 0) --idx;
            return v[std::min(idx, v.size() - 1)] / 1000.0;
        };
        s.p50_ms = pct(0.50); s.p95_ms = pct(0.95); s.p99_ms = pct(0.99);
        return s;
    }
private:
    static constexpr size_t kCap = 1024;
    mutable std::mutex    mu_;
    std::vector<uint32_t> buf_;
    size_t   next_ = 0;
    uint64_t count_ = 0;
    uint32_t max_us_ = 0;
};

} // namespace visualmemory

#endif // VISUALMEMORY_LATENCY_RING_H
