// SPDX-License-Identifier: MIT
//
// libvisualmemory — public value types.
//
// This header intentionally depends ONLY on the C++ standard library. It must
// never include Argus, OpenCV, SQLite, TurboJPEG, RKNN or any platform header,
// so the library stays reusable by any C++20 extension.
#ifndef VISUALMEMORY_TYPES_H
#define VISUALMEMORY_TYPES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace visualmemory {

// Pixel layout of a submitted frame. Only the formats listed here are accepted
// by the encoder; anything else is rejected at submit time.
enum class PixelFormat : uint8_t {
    RGB24,  // packed R,G,B    — 3 bytes/pixel, single plane
    BGR24,  // packed B,G,R    — 3 bytes/pixel, single plane (Argus capture format)
    NV12,   // planar Y + interleaved UV (4:2:0), two planes
    JPEG,   // already-encoded JPEG bytes — stored verbatim
};

// Metadata that travels with every frame. All fields are owned (std::string),
// so nothing borrows from the caller after submission.
struct FrameMetadata {
    std::string source_id;      // e.g. "/dev/video1", "rtsp-camera-1", "file:test.mp4"
    std::string source_type;    // "usb" | "rtsp" | "file" | "presentation" | ...
    std::string device_id;      // player / device identifier
    int64_t     capture_utc_ms = 0;  // wall-clock (system_clock) epoch ms — used for queries
    int64_t     pts_ns         = 0;  // media PTS where available (NOT equivalent to UTC)
    uint64_t    source_sequence = 0; // monotonic per-source frame counter
};

// A fully-owned frame handed to the library. After try_submit() consumes it
// (via std::move) the library owns the pixel buffer for its entire lifetime.
struct OwnedFrame {
    int         width         = 0;
    int         height        = 0;
    PixelFormat pixel_format  = PixelFormat::BGR24;
    int         stride0       = 0;   // bytes per row, plane 0 (0 => tightly packed)
    int         stride1       = 0;   // bytes per row, plane 1 (NV12 UV plane)
    std::vector<uint8_t> pixels;     // owned pixel bytes (all planes, plane0 then plane1)
    FrameMetadata metadata;
};

// Stage 2 (optional): asynchronous embedding + vector search. Everything here is
// inert unless `enabled` is true AND an EmbeddingProvider is injected with
// VisualMemory::set_embedding_provider() before start().
struct EmbeddingOptions {
    bool     enabled = false;            // OFF by default
    size_t   max_pending = 4;            // bounded embedding queue (frame id + path only, ~100 B each)
    int      retention_days = 7;         // embedding lifetime — independent of Config::retention_hours (JPEGs)
    bool     backfill_on_startup = false;// embed already-stored JPEGs that have no vector yet
    int      backfill_batch = 8;         // frames per backfill cycle (bounded work per cycle)
    int64_t  backfill_pause_ms = 250;    // idle pause between backfill cycles (yields CPU/NPU)
    // Bounds for the startup backfill (only used when backfill_on_startup is true). It is deliberately NOT open
    // ended: a store with thousands of old frames must not occupy the NPU/SD card unbounded after a restart.
    // Newest frames are embedded first. 0 = no limit on that dimension (an explicit opt-in).
    uint64_t backfill_max_frames = 500;    // total frames attempted per process run
    int      backfill_max_age_hours = 24;  // only frames captured within this many hours
    int      max_attempts = 2;           // inference attempts per frame for recoverable errors
    bool     prefer_sqlite_vec = true;   // false (or sqlite-vec not compiled in) => BLOB + C++ cosine fallback
    size_t   max_jpeg_bytes = 8u << 20;  // refuse to decode larger stored files (bounds memory)
};

// Library configuration. Values are validated in VisualMemory::start().
struct Config {
    std::string root_directory;                 // storage root (DB + frames live here)

    // Sampling cadence is enforced by the *caller/adapter*, not the library.
    // Kept here so an adapter can read one consistent config object.
    int64_t sample_interval_ms = 5000;

    // Stored image bounding box. Frames are scaled to fit inside this box while
    // preserving aspect ratio (no stretching). Encoded dimensions may be smaller.
    int stored_width  = 640;
    int stored_height = 360;
    int jpeg_quality  = 75;                      // 1..100

    size_t   max_pending_frames = 4;             // bounded ingest queue depth
    uint64_t max_storage_bytes  = 4ULL * 1000 * 1000 * 1000; // logical JPEG quota (~4 GB)
    int      retention_hours    = 48;            // delete frames older than this

    int64_t  retention_interval_ms = 60'000;     // how often the worker runs retention
    // Retention runs in short slices between frames (never one long pass), so an expired backlog
    // cannot stall the ingest queue. A slice ends after this many ms or deletions, whichever first.
    int64_t  retention_slice_ms = 100;
    uint64_t retention_slice_deletions = 64;
    bool     compute_sha256 = true;              // hash stored JPEG bytes for integrity

    EmbeddingOptions embedding;                  // Stage 2 (disabled by default)
};

// One row returned by a query.
struct FrameInfo {
    int64_t       id = 0;
    FrameMetadata metadata;
    int           width  = 0;
    int           height = 0;
    std::string   relative_jpeg_path;            // relative to Config::root_directory
    uint64_t      file_size_bytes = 0;
    std::string   sha256_hex;                    // empty if hashing disabled
    int64_t       created_utc_ms = 0;
    bool          image_available = true;        // false once the JPEG expired but its embedding/metadata is kept
};

// Non-owning view of a decoded 8-bit image, used for embedding queries.
// Only RGB24 and BGR24 (packed, 3 bytes/pixel) are accepted.
struct ImageView {
    const uint8_t* data   = nullptr;
    int            width  = 0;
    int            height = 0;
    int            stride = 0;                    // bytes per row; 0 => tightly packed
    PixelFormat    format = PixelFormat::RGB24;
};

// ---- Stage 2: vector search -------------------------------------------------
struct SearchOptions {
    int         top_k = 5;                        // clamped to [1, 100]
    int64_t     start_utc_ms = 0;                 // inclusive lower bound on capture time (0 = unbounded)
    int64_t     end_utc_ms   = 0;                 // exclusive upper bound            (0 = unbounded)
    std::string source_id;                        // empty = all sources
    bool        has_min_similarity = false;       // thresholds are opt-in: no unvalidated default
    float       min_similarity = 0.0f;            // cosine similarity in [-1,1]; applied when has_min_similarity
    bool        require_image = false;            // drop hits whose JPEG is unavailable/missing
};

struct SearchResult {
    int64_t       frame_id = 0;                   // stable observation id (frames.id)
    FrameMetadata metadata;                       // source, capture time, ...
    std::string   relative_jpeg_path;
    std::string   absolute_path;                  // resolved against root_directory
    bool          image_available = true;         // DB flag (false => JPEG retention already expired)
    bool          image_exists    = true;         // file actually present on disk right now
    int64_t       model_id = 0;                   // embedding space the score belongs to
    float         similarity = 0.0f;              // cosine similarity (higher = more similar)
    float         distance   = 0.0f;              // 1 - similarity
};

struct SearchStatus {
    enum class Code { Ok, NotEnabled, DimensionMismatch, InvalidVector, ModelMismatch,
                      ProviderFailed, InvalidArgument, DatabaseError,
                      Cancelled /* interrupted via cancel_search() / deadline */ };
    Code        code = Code::Ok;
    std::string message;
    uint64_t    candidates_scanned = 0;           // rows the vector scan considered (0 if backend can't tell)
    double      elapsed_ms = 0;
    bool ok() const { return code == Code::Ok; }
};

// Latency summary over the most recent samples (ring of up to 1024 values per
// stage, so a 10-minute run at the default 5 s cadence is fully covered).
struct LatencyStats {
    uint64_t count  = 0;   // samples recorded since start (may exceed ring size)
    double   p50_ms = 0;
    double   p95_ms = 0;
    double   p99_ms = 0;
    double   max_ms = 0;
};

// Runtime counters (all monotonic except queue_depth / bytes_on_disk).
// The most recent SQLite failure of the frame writer (diagnostics for the cumulative `database_failures` counter:
// lets a historical transient failure be told apart from an active one by comparing utc_ms with last_commit_utc_ms).
struct DbErrorInfo {
    uint64_t    count = 0;              // failures recorded since start (equals Stats::database_failures)
    int         code = 0;               // sqlite3_errcode() at the failure (0 = none yet; -1 = database not open)
    int         extended_code = 0;      // sqlite3_extended_errcode()
    std::string code_name;              // e.g. "SQLITE_BUSY"
    std::string message;                // sqlite3_errmsg()
    std::string operation;              // "insert.prepare" | "insert.step" | "insert.not_open"
    int64_t     utc_ms = 0;             // wall clock of the most recent failure (0 = none)
    int64_t     frame_sequence = 0;     // source_sequence of the frame that was lost
};

struct Stats {
    uint64_t samples_seen         = 0;  // frames handed to try_submit()
    uint64_t accepted             = 0;  // frames queued for processing
    uint64_t dropped_queue_full   = 0;  // rejected because the queue was full
    uint64_t dropped_invalid      = 0;  // rejected validation (bad dims/strides/format)
    uint64_t encoded              = 0;  // frames successfully encoded + persisted
    uint64_t jpeg_write_failures  = 0;
    uint64_t database_failures    = 0;
    DbErrorInfo last_database_error;    // details of the most recent database failure
    uint64_t retention_deletions  = 0;
    uint64_t bytes_on_disk        = 0;  // logical sum of stored JPEG bytes
    int64_t  last_commit_utc_ms   = 0;  // wall-clock time the newest JPEG+row was committed (0 = none yet)
    bool     retention_pending    = false; // retention still has backlog to work through (sliced)
    uint64_t retention_slices     = 0;
    size_t   queue_depth          = 0;  // instantaneous pending frames
    size_t   max_queue_depth      = 0;  // high-water mark since start

    // Worker-thread stage latencies (per stored frame).
    LatencyStats encode;        // BGR/NV12 -> downscale -> TurboJPEG
    LatencyStats file_write;    // atomic JPEG write incl. fsync + rename (SD-card latency)
    LatencyStats db_insert;     // SQLite INSERT (FULL sync)
    LatencyStats process_total; // whole per-frame pipeline on the worker
    LatencyStats retention_slice; // one bounded retention slice on the worker (should stay ~retention_slice_ms)
    LatencyStats commit_delay;  // capture -> committed (queue wait + process); rises when the writer is stalled
};

// Stage 2 counters (all zero when embeddings are disabled).
struct EmbeddingStats {
    bool     active = false;                     // provider ready and worker running
    std::string status;                          // "disabled" | "no provider" | "provider not ready: ..." | "active"
    std::string backend;                         // "sqlite-vec <ver>" | "blob-cosine-fallback"
    int64_t  model_id = 0;
    int      dimension = 0;
    uint64_t enqueued = 0;
    uint64_t dropped_queue_full = 0;             // dropped from the live queue (backfill will pick them up)
    uint64_t embedded = 0;                       // vectors persisted
    uint64_t backfilled = 0;                     // subset of `embedded` produced by backfill
    uint64_t backfill_attempted = 0;             // frames the startup backfill has processed this run
    uint64_t backfill_cap = 0;                   // configured backfill_max_frames (0 = unlimited / backfill off)
    bool     backfill_done = false;              // cap reached or nothing left within the age window
    uint64_t retries = 0;
    uint64_t inference_failures = 0;             // provider returned false
    uint64_t invalid_vectors = 0;                // NaN/Inf/zero-norm/wrong dimension
    uint64_t decode_failures = 0;                // missing/corrupt JPEG
    uint64_t failed_permanent = 0;               // marked failed in the DB
    uint64_t db_failures = 0;
    uint64_t searches = 0;
    uint64_t vectors_stored = 0;                 // rows in the active model's vector table
    uint64_t vector_bytes = 0;                   // logical vector payload (dimension*4 per row)
    size_t   queue_depth = 0;
    size_t   max_queue_depth = 0;
    LatencyStats file_read, jpeg_decode, preprocess, inference, vector_insert, process_total;
    LatencyStats search;                         // end-to-end search_similar() latency
};

} // namespace visualmemory

#endif // VISUALMEMORY_TYPES_H
