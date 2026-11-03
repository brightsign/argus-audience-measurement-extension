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
    bool     compute_sha256 = true;              // hash stored JPEG bytes for integrity
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
};

// Runtime counters (all monotonic except queue_depth / bytes_on_disk).
struct Stats {
    uint64_t samples_seen         = 0;  // frames handed to try_submit()
    uint64_t accepted             = 0;  // frames queued for processing
    uint64_t dropped_queue_full   = 0;  // rejected because the queue was full
    uint64_t dropped_invalid      = 0;  // rejected validation (bad dims/strides/format)
    uint64_t encoded              = 0;  // frames successfully encoded + persisted
    uint64_t jpeg_write_failures  = 0;
    uint64_t database_failures    = 0;
    uint64_t retention_deletions  = 0;
    uint64_t bytes_on_disk        = 0;  // logical sum of stored JPEG bytes
    size_t   queue_depth          = 0;  // instantaneous pending frames
};

} // namespace visualmemory

#endif // VISUALMEMORY_TYPES_H
