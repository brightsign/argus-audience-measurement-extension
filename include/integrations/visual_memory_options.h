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

// Stage 2 (optional): MobileNetV2 embedding + historical vector search. OFF by default.
struct VisualEmbeddingOptions {
    bool        enabled = false;
    std::string backend = "rknn";                                   // "rknn" (NPU) | "mock" (tests/dev only)
    std::string model_path = "model/visual_memory/mobilenet_embedder.rknn";   // relative to the extension dir
    size_t      max_pending = 4;                                    // bounded embedding queue
    int         retention_days = 7;                                 // embedding lifetime, independent of retention_hours
    bool        backfill_on_startup = false;                        // embed stored JPEGs that lack a vector
    uint64_t    backfill_max_frames = 500;                          // cap on frames attempted per run (0 = unlimited)
    int         backfill_max_age_hours = 24;                        // only frames captured this recently (0 = no age limit)
    int         npu_core = -1;                                      // -1 auto, 0..2 single core, 3 all
    bool        prefer_sqlite_vec = true;                           // false => BLOB + C++ cosine fallback
};

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
    // Stage 2 runs only when BOTH vector_search_enabled and embedding.enabled are true (and the binary
    // was built with -DENABLE_VISUAL_EMBEDDINGS=ON). Both default to false.
    bool        vector_search_enabled = false;
    VisualEmbeddingOptions embedding;

    // Metadata recorded with every stored frame (filled from AppConfig).
    std::string source_id;
    std::string source_type;
    std::string device_id;
};

} // namespace argus

#endif // ARGUS_VISUAL_MEMORY_OPTIONS_H
