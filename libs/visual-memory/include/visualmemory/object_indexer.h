// SPDX-License-Identifier: MIT
// ObjectIndexer (D4 automatic indexing): ONE background worker that
//   stored JPEG (camera frame or Weston/VOP2 display frame) -> ObjectDetector -> allowlist/size/duplicate filter -> crop ->
//   MobileNetV2 embedding -> ObjectIndex (provenance + crop JPEG)  and records the result per frame (restart-safe progress).
// The SAME detector, embedding provider, class allowlist and persistence serve every source. Each source has its own bounded
// queue, rate limit and cursor; scheduling is fair (live jobs of the sources in turn, a backfill only when no live job is due),
// so a historical backfill cannot starve live processing and one source cannot starve the other.
// Frames are read through ObjectFrameSource (the read-only VisualMemoryReader for the camera, the Proof-of-Play store for the
// display); the worker never touches the capture path, the frame database or the Proof-of-Play verification. Work arrives through a bounded queue (live: newest unprocessed frames, rate limited; backfill: an
// explicit, capped request). Everything heavy (decode, detection, embedding, SQLite) happens on the worker thread;
// the public methods only take short locks, so dashboard requests are never blocked by detection.
#ifndef VISUALMEMORY_OBJECT_INDEXER_H
#define VISUALMEMORY_OBJECT_INDEXER_H

#include "visualmemory/embedding_provider.h"
#include "visualmemory/object_detector.h"
#include "visualmemory/object_index.h"
#include "visualmemory/reader.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace visualmemory {

struct ObjectIndexerConfig {
    bool   live_scan = true;               // index newly stored frames as they arrive (else backfill only)
    std::vector<int> class_ids;            // allowlist (COCO ids); empty = nothing is indexed
    float  min_confidence = 0.30f;         // worker-side detection threshold (does not touch Argus' YOLOX)
    int    max_objects_per_frame = 8;
    int    min_box_px = 24;                // shorter box side in stored-image pixels
    double max_duplicate_iou = 0.70;       // same-region boxes of different classes: keep the more confident
    int    frames_per_minute = 12;         // sustained live rate (1..600)
    int    queue_depth = 8;                // bounded work queue (1..64)
    int    poll_ms = 2000;
    int    backfill_pause_ms = 150;        // courtesy pause between backfill frames (NPU / CPU headroom)
    int    backoff_ms = 5000;              // after a detector / embedding provider failure
    int    max_backfill_frames = 100;      // hard cap per request
    std::string detector_tag;              // overrides detector.name() for the progress key (tests)
};

struct BackfillRequest {
    std::string source = "camera";                 // which source to backfill: "camera" | "display"
    int64_t start_utc_ms = 0, end_utc_ms = 0;      // frame capture time range (0 = unbounded)
    std::vector<int64_t> frame_ids;                // explicit camera frames, or display frames of kind `kind` (if non-empty the range is ignored)
    std::string kind = "history";                  // display only: kind of the explicit ids ("history" | "checkpoint")
    int     limit = 50;                            // clamped to [1, max_backfill_frames]
    bool    retry_failed = false;                  // also redo frames whose earlier attempt failed
    bool    newest_first = true;
};

struct IndexerSourceStats {
    std::string type;                              // "camera" | "display"
    bool        live_scan = false;
    int         frames_per_minute = 0, queue_depth = 0, queue_capacity = 0;
    uint64_t    frames_processed = 0, frames_with_objects = 0, objects_indexed = 0, frames_image_unavailable = 0, frames_failed = 0;
    uint64_t    skipped_stale = 0, dropped_queue_full = 0;
    int64_t     cursor_utc_ms = 0;
    double      avg_detect_ms = 0, avg_embed_ms = 0, avg_index_ms = 0, avg_frame_total_ms = 0;   // moving averages of THIS source (embed: per crop)
    std::string store_state;                       // "open" | the reason the frame store is unavailable
};

struct ObjectIndexerStats {
    std::string state = "disabled";                // disabled|starting|idle|running|backoff|error|stopped
    std::string last_error;
    int64_t     last_error_utc_ms = 0;
    uint64_t    frames_processed = 0, frames_with_objects = 0, objects_indexed = 0, objects_duplicate = 0;
    uint64_t    frames_image_unavailable = 0, frames_failed = 0;
    uint64_t    skipped_stale = 0;                 // live frames not examined because the worker was behind (sampled out)
    uint64_t    detector_errors = 0, provider_errors = 0, db_errors = 0;
    int         queue_depth = 0, queue_capacity = 0;
    uint64_t    dropped_queue_full = 0;
    double      last_detect_ms = 0, avg_detect_ms = 0, last_index_ms = 0, avg_index_ms = 0, last_frame_total_ms = 0;
    double      last_embed_ms = 0, avg_embed_ms = 0;   // MobileNetV2 embedding time PER CROP (moving average)
    int64_t     cursor_utc_ms = 0;                 // live scan position (frames newer than this are candidates)
    bool        live_scan = false;
    std::string detector, detector_version;
    std::vector<IndexerSourceStats> sources;       // per source (camera, display); the fields above are totals over all sources
    // backfill
    std::string backfill_source;                   // source of the current / last backfill
    std::string backfill_state = "none";           // none|queued|running|done|cancelled|error
    int         backfill_total = 0, backfill_done = 0, backfill_indexed = 0, backfill_failed = 0;
    int64_t     backfill_started_utc_ms = 0, backfill_finished_utc_ms = 0;
};

// One frame source served by the worker.
struct IndexerSource {
    ObjectFrameSource* source = nullptr;           // must outlive the indexer
    bool live_scan = false;                        // index newly stored frames of this source as they arrive
    int  frames_per_minute = 12;                   // sustained live rate of THIS source (1..600)
    int  queue_depth = 8;                          // bounded live queue of THIS source (1..64)
};

class ObjectIndexer {
public:
    using ProviderFn = std::function<std::shared_ptr<EmbeddingProvider>(std::string& why)>;
    using DetectorFn = std::function<std::shared_ptr<ObjectDetector>(std::string& why)>;

    // Several sources (camera and/or display) sharing one detector, one embedding provider and one object index.
    ObjectIndexer(ObjectIndexerConfig cfg, ObjectIndex& index, std::vector<IndexerSource> sources, ProviderFn provider, DetectorFn detector);
    // `index` and `reader` must outlive the indexer. The provider and detector are obtained lazily on the worker
    // thread (the first frame), so construction and start() never load a model.
    ObjectIndexer(ObjectIndexerConfig cfg, ObjectIndex& index, VisualMemoryReader& reader, ProviderFn provider, DetectorFn detector);
    ~ObjectIndexer();
    ObjectIndexer(const ObjectIndexer&) = delete;
    ObjectIndexer& operator=(const ObjectIndexer&) = delete;

    void start();                      // idempotent
    void stop();                       // idempotent; joins the worker (an in-flight frame finishes first)

    // Queue a bounded backfill. Returns false (err set) when one is already queued/running or the request is invalid.
    bool request_backfill(const BackfillRequest& r, std::string& err);
    void cancel_backfill();

    ObjectIndexerStats stats() const;
    const ObjectIndexerConfig& config() const { return cfg_; }
    bool has_source(const std::string& type) const;

private:
    struct Impl;
    ObjectIndexerConfig cfg_;
    std::unique_ptr<ObjectFrameSource> owned_camera_;   // set by the VisualMemoryReader constructor
    std::unique_ptr<Impl> impl_;
};

} // namespace visualmemory

#endif // VISUALMEMORY_OBJECT_INDEXER_H
