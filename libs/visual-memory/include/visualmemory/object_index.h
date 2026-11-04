// SPDX-License-Identifier: MIT
// ObjectIndex (D4): manually selected object regions of stored frames, embedded with the SAME model as full
// frames but kept in a SEPARATE vector space/table, so crop vectors are never compared with full-frame vectors.
//
// Storage: <root>/objects.db (own versioned schema, PRAGMA user_version). It is a separate SQLite file next to
// visual_memory.db on purpose: the Argus writer keeps sole ownership of visual_memory.db (no lock contention, no
// schema change to Stage 1/2 data), while the dashboard API process owns objects.db. region.frame_id refers to
// frames.id of visual_memory.db (checked on insert through the reader). Original frame JPEGs are never modified;
// crops are regenerated from the frame on demand (no crop file is stored).
//
// Independent of Argus. Standard library + the other libvisualmemory headers only.
#ifndef VISUALMEMORY_OBJECT_INDEX_H
#define VISUALMEMORY_OBJECT_INDEX_H

#include "visualmemory/embedding_provider.h"
#include "visualmemory/reader.h"
#include "visualmemory/types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace visualmemory {

// Rectangle in the pixel coordinates of the STORED JPEG (frames.stored_width/height), origin top-left.
struct Roi { int x = 0, y = 0, w = 0, h = 0; };

enum class RoiCheck { Ok, Invalid, TooSmall, OutOfBounds };
const char* to_string(RoiCheck c);
// Rejects negative origin, non-positive size, rectangles leaving the image, and crops smaller than min_px per side.
RoiCheck validate_roi(const Roi& r, int image_w, int image_h, int min_px = 16);

struct ObjectIndexConfig {
    std::string root_directory;                  // same directory as the frame store
    int         busy_timeout_ms = 3000;
    int         min_roi_px = 16;                 // smallest accepted crop side (original-image pixels)
    uint64_t    max_label_bytes = 64;
};

struct ObjectRegion {
    int64_t     region_id = 0;
    int64_t     frame_id = 0;
    std::string source_id;
    int64_t     capture_utc_ms = 0;              // of the parent frame
    Roi         roi;
    int         image_width = 0, image_height = 0;   // stored size of the parent JPEG the ROI refers to
    std::string label;                           // optional, user supplied
    int64_t     model_id = 0;
    int         embedding_status = 1;            // 1 ok
    bool        has_crop_file = false;           // always false: crops are regenerated
    int64_t     created_utc_ms = 0;
    std::string frame_sha256;                    // hash of the parent JPEG at indexing time (audit)
};

struct ObjectIndexResult {
    enum class Code { Indexed, Duplicate, FrameNotFound, ImageUnavailable, InvalidRoi, DecodeFailed,
                      ProviderFailed, InvalidVector, DatabaseError, InvalidArgument, NotOpen };
    Code        code = Code::NotOpen;
    std::string message;
    int64_t     region_id = 0;
    double      elapsed_ms = 0;
    bool ok() const { return code == Code::Indexed || code == Code::Duplicate; }
};

struct ObjectSearchOptions {
    int         top_k = 5;                       // clamped to [1, 100]
    int64_t     start_utc_ms = 0, end_utc_ms = 0;
    std::string source_id;
    std::string label;                           // exact match, empty = any
    bool        has_min_similarity = false;
    float       min_similarity = 0.0f;
    bool        require_image = false;           // drop hits whose parent JPEG is unavailable
    int64_t     exclude_region_id = 0;
};

struct ObjectHit {
    ObjectRegion region;
    float similarity = 0.0f, distance = 0.0f;    // cosine; NOT a calibrated identification probability
    bool  image_available = false;               // parent frame JPEG still retained (DB flag)
    bool  image_exists = false;                  // file present right now
};

struct ObjectSearchResult {
    SearchStatus status;                         // code Ok with hits empty => empty index / no match
    std::vector<ObjectHit> hits;
    EmbeddingSpec space;                         // embedding space the scores belong to
    uint64_t index_size = 0;                     // regions in that space
};

struct ObjectFilter { int64_t start_utc_ms = 0, end_utc_ms = 0; std::string source_id, label; int64_t frame_id = 0; };

struct ObjectIndexSummary {
    bool open = false;
    int  schema_version = 0;
    uint64_t regions = 0, vector_bytes = 0, db_bytes = 0;
    int64_t oldest_capture_utc_ms = 0, newest_capture_utc_ms = 0;
    std::vector<EmbeddingSpec> models;
};

class ObjectIndex {
public:
    explicit ObjectIndex(ObjectIndexConfig cfg);
    ~ObjectIndex();
    ObjectIndex(const ObjectIndex&) = delete;
    ObjectIndex& operator=(const ObjectIndex&) = delete;

    // Opens/creates objects.db and migrates its schema (never touches visual_memory.db).
    bool open(std::string& err);
    bool is_open() const;
    void close();

    // Index a region of a stored frame: read JPEG -> decode -> crop (aspect ratio preserved) -> provider.embed
    // (pp1 preprocessing inside the provider) -> L2 normalise -> persist. Re-indexing the same frame/ROI/model
    // returns Duplicate with the existing region_id (the label is updated when a new one is given).
    ObjectIndexResult index_region(VisualMemoryReader& frames, EmbeddingProvider& provider, int64_t frame_id,
                                   const Roi& roi, const std::string& label);

    // Crop of a stored frame as JPEG (regenerated; the stored frame is untouched). Return codes as
    // VisualMemoryReader::read_jpeg (0 ok, 1 unknown frame, 2 expired, 3 unreadable) plus 4 invalid ROI, 5 decode.
    int crop_jpeg(VisualMemoryReader& frames, int64_t frame_id, const Roi& roi, int max_side, int quality,
                  std::vector<uint8_t>& out, std::string& err);
    int region_crop_jpeg(VisualMemoryReader& frames, int64_t region_id, int max_side, int quality,
                         std::vector<uint8_t>& out, std::string& err);

    // Searches (object vectors only). `frames` (optional) is used to report image availability.
    ObjectSearchResult search_by_roi(VisualMemoryReader& frames, EmbeddingProvider& provider, int64_t frame_id,
                                     const Roi& roi, const ObjectSearchOptions& o);
    ObjectSearchResult search_by_jpeg(VisualMemoryReader* frames, EmbeddingProvider& provider,
                                      const std::vector<uint8_t>& jpeg, const ObjectSearchOptions& o);
    ObjectSearchResult search_by_region(VisualMemoryReader* frames, int64_t region_id, const ObjectSearchOptions& o);
    ObjectSearchResult search_by_vector(VisualMemoryReader* frames, const std::vector<float>& v,
                                        const EmbeddingSpec& space, const ObjectSearchOptions& o);

    bool get_region(int64_t region_id, ObjectRegion& out);
    std::vector<ObjectRegion> list_regions(const ObjectFilter& f, int limit, int offset, bool newest_first,
                                           uint64_t* total = nullptr);
    bool set_label(int64_t region_id, const std::string& label);
    bool delete_region(int64_t region_id);
    ObjectIndexSummary summary();

    // Independent retention of the object index. Regions are explicit user choices, so retention follows the time
    // they were INDEXED (created_at), not the parent frame's capture time (a historical frame must not be erased
    // the moment it is indexed). Drops regions indexed before `older_than_created_ms` (0 = skip) and, when
    // max_regions > 0, the oldest-indexed regions beyond that count. Returns rows deleted.
    uint64_t prune(int64_t older_than_created_ms, uint64_t max_regions);

    // Interrupt a search running on another thread (returns Code::Cancelled).
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace visualmemory

#endif // VISUALMEMORY_OBJECT_INDEX_H
