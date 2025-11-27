#ifndef SHARED_FRAME_H
#define SHARED_FRAME_H

#include <cstdint>
#include <vector>
#include <memory>

/**
 * SharedFrame: A frame captured once and fanned out to multiple model workers.
 * 
 * Allocated once in capture thread, read-only for workers.
 * Wrapped in shared_ptr for safe access across threads.
 */
struct SharedFrame {
    int64_t  pts_ns{0};      ///< Presentation timestamp (nanoseconds)
    uint64_t seq{0};         ///< Sequence number for ordering

    int width{0};            ///< Frame width in pixels (preprocessed/resized)
    int height{0};           ///< Frame height in pixels (preprocessed/resized)
    
    int orig_width{0};       ///< Original camera/stream width before preprocessing
    int orig_height{0};      ///< Original camera/stream height before preprocessing

    // BGR24 packed, CPU memory (plane0 only, single-planar)
    // Captured once by capture thread, read-only for model threads
    std::vector<uint8_t> bgr;
};

#endif // SHARED_FRAME_H
