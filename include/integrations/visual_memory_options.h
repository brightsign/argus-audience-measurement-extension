// SPDX-License-Identifier: MIT
// Plain options describing how Argus should drive libvisualmemory. Kept header
// light (standard types only) so it can be included from orchestrator.h without
// pulling in the library or its dependencies.
#ifndef ARGUS_VISUAL_MEMORY_OPTIONS_H
#define ARGUS_VISUAL_MEMORY_OPTIONS_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace argus {

struct VisualMemoryOptions {
    bool        enabled = false;                 // OFF by default — product default
    std::string storage_root = "/storage/sd/visual-memory";
    int64_t     sample_interval_ms = 5000;
    int         stored_width  = 640;
    int         stored_height = 360;
    int         jpeg_quality  = 75;
    size_t      max_pending_frames = 4;
    uint64_t    max_storage_bytes  = 4'000'000'000ULL;
    int         retention_hours = 48;
    bool        vector_search_enabled = false;   // Stage 2 placeholder (ignored now)

    // Metadata recorded with every stored frame (filled from AppConfig).
    std::string source_id;
    std::string source_type;
    std::string device_id;
};

} // namespace argus

#endif // ARGUS_VISUAL_MEMORY_OPTIONS_H
