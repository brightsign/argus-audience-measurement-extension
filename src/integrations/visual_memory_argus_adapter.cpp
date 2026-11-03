// SPDX-License-Identifier: MIT
#include "integrations/visual_memory_argus_adapter.h"

#include "metrics/log_global.h"
#include "visualmemory/visual_memory.h"

#include <chrono>
#include <cstring>

namespace argus {

namespace {
int64_t now_utc_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
int64_t now_steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

struct VisualMemoryAdapter::Impl {
    std::unique_ptr<visualmemory::VisualMemory> vm;
    int64_t last_sample_steady_ms = 0;
    bool    first = true;
    // Rate-limit the "queue full" warning so a stalled card can't spam the log.
    int64_t last_drop_warn_ms = 0;
};

VisualMemoryAdapter::VisualMemoryAdapter(VisualMemoryOptions opt)
    : opt_(std::move(opt)), impl_(std::make_unique<Impl>()) {}

VisualMemoryAdapter::~VisualMemoryAdapter() { stop(); }

bool VisualMemoryAdapter::start() {
    if (!opt_.enabled) {
        LG_INFO("visual_memory: disabled (storage OFF by default)");
        return true;
    }
    visualmemory::Config cfg;
    cfg.root_directory     = opt_.storage_root;
    cfg.sample_interval_ms = opt_.sample_interval_ms;
    cfg.stored_width       = opt_.stored_width;
    cfg.stored_height      = opt_.stored_height;
    cfg.jpeg_quality       = opt_.jpeg_quality;
    cfg.max_pending_frames = opt_.max_pending_frames;
    cfg.max_storage_bytes  = opt_.max_storage_bytes;
    cfg.retention_hours    = opt_.retention_hours;

    impl_->vm = std::make_unique<visualmemory::VisualMemory>(std::move(cfg));
    if (!impl_->vm->start()) {
        // Degrade Visual Memory only — Argus inference must keep running.
        LG_ERROR("visual_memory: failed to start at '%s'; continuing without frame storage",
                 opt_.storage_root.c_str());
        impl_->vm.reset();
        opt_.enabled = false;
        return false;
    }
    LG_INFO("visual_memory: enabled root='%s' interval=%lldms %dx%d q=%d retention=%dh",
            opt_.storage_root.c_str(), (long long)opt_.sample_interval_ms,
            opt_.stored_width, opt_.stored_height, opt_.jpeg_quality, opt_.retention_hours);
    return true;
}

void VisualMemoryAdapter::stop() {
    if (impl_ && impl_->vm) {
        impl_->vm->stop();
        impl_->vm.reset();
    }
}

void VisualMemoryAdapter::offer_bgr(const uint8_t* bgr, int width, int height,
                                    int64_t pts_ns, uint64_t seq) noexcept {
    if (!opt_.enabled || !impl_ || !impl_->vm || bgr == nullptr || width <= 0 || height <= 0)
        return;

    // Sampling gate — steady_clock so accelerated file playback still samples at
    // real wall-clock cadence. First frame is always taken.
    const int64_t now = now_steady_ms();
    if (!impl_->first &&
        (now - impl_->last_sample_steady_ms) < opt_.sample_interval_ms) {
        return;
    }
    impl_->first = false;
    impl_->last_sample_steady_ms = now;

    // One bounded copy of the full-resolution source scene.
    visualmemory::OwnedFrame f;
    f.width = width;
    f.height = height;
    f.pixel_format = visualmemory::PixelFormat::BGR24;
    f.stride0 = width * 3;
    const size_t n = static_cast<size_t>(width) * height * 3;
    try {
        f.pixels.resize(n);
    } catch (...) {
        return;  // low memory: skip this sample, never crash the capture thread
    }
    std::memcpy(f.pixels.data(), bgr, n);

    f.metadata.source_id       = opt_.source_id;
    f.metadata.source_type     = opt_.source_type;
    f.metadata.device_id       = opt_.device_id;
    f.metadata.capture_utc_ms  = now_utc_ms();
    f.metadata.pts_ns          = pts_ns;
    f.metadata.source_sequence = seq;

    if (!impl_->vm->try_submit(std::move(f))) {
        const int64_t wall = now_utc_ms();
        if (wall - impl_->last_drop_warn_ms > 30'000) {
            impl_->last_drop_warn_ms = wall;
            auto s = impl_->vm->stats();
            LG_WARN("visual_memory: sample dropped (queue full); encoded=%llu dropped=%llu",
                    (unsigned long long)s.encoded,
                    (unsigned long long)s.dropped_queue_full);
        }
    }
}

} // namespace argus
