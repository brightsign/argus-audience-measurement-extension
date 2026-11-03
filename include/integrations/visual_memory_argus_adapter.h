// SPDX-License-Identifier: MIT
// Thin Argus-side adapter around libvisualmemory. It owns the sampling decision
// and the one unavoidable frame copy; all encoding, SQLite and filesystem work
// happens on the library's worker thread. The library itself has no Argus
// dependency — this translation unit is the only bridge.
#ifndef ARGUS_VISUAL_MEMORY_ARGUS_ADAPTER_H
#define ARGUS_VISUAL_MEMORY_ARGUS_ADAPTER_H

#include "integrations/visual_memory_options.h"

#include <cstdint>
#include <memory>

namespace argus {

class VisualMemoryAdapter {
public:
    explicit VisualMemoryAdapter(VisualMemoryOptions opt);
    ~VisualMemoryAdapter();

    VisualMemoryAdapter(const VisualMemoryAdapter&) = delete;
    VisualMemoryAdapter& operator=(const VisualMemoryAdapter&) = delete;

    // Start the library worker. If disabled, returns true and stays inert.
    // A storage/init failure logs and returns false WITHOUT throwing, so Argus
    // continues in normal tracking mode.
    bool start();
    void stop();

    bool enabled() const { return opt_.enabled; }

    // Offer a full-resolution BGR24 source frame from the capture thread. At
    // most one frame per sample_interval_ms is copied and queued; everything
    // else returns immediately. Never blocks on I/O, SQL or the worker queue.
    void offer_bgr(const uint8_t* bgr, int width, int height,
                   int64_t pts_ns, uint64_t seq) noexcept;

private:
    struct Impl;
    VisualMemoryOptions   opt_;
    std::unique_ptr<Impl> impl_;
};

} // namespace argus

#endif // ARGUS_VISUAL_MEMORY_ARGUS_ADAPTER_H
