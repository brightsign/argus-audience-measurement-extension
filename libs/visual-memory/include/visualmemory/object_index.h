// SPDX-License-Identifier: MIT
// ObjectIndex (D4): manually selected object regions of stored frames, embedded with the SAME model as full
// frames but kept in a SEPARATE vector space/table, so crop vectors are never compared with full-frame vectors.
//
// Schema v3 (over v2): objects can come from two frame sources, "camera" (Visual Memory frames) and "display" (Weston/VOP2 frames
// kept by Proof of Play). A source frame is identified by (source_type, source_kind, frame_id): camera and Proof-of-Play ids are from
// different databases and are never joined on the id alone. Display objects keep the Proof-of-Play session, expected/reported
// advertisement, playback offset, timestamp uncertainty and provenance of their frame. v3 rebuilds object_regions and
// object_frame_progress (their UNIQUE/PRIMARY KEY now include the source) preserving every row and region_id; a copy of the v2 file is
// kept as objects.db.pre-v3.
//
// Schema v2 (additive over v1; every v1 row is kept and reads back as index_origin='manual'): automatic indexing
// provenance (index_origin, class, detector confidence, detector/model version), independently retained crop JPEGs
// (<root>/object_crops/<region_id>.jpg, own age + quota policy) and a per-frame processing-progress table.
//
// Storage: <root>/objects.db (own versioned schema, PRAGMA user_version). It is a separate SQLite file next to
// visual_memory.db on purpose: the Argus writer keeps sole ownership of visual_memory.db (no lock contention, no
// schema change to Stage 1/2 data), while the dashboard API process owns objects.db. region.frame_id refers to
// frames.id of visual_memory.db (checked on insert through the reader). Original frame JPEGs are never modified;
// a region's crop is stored as its own small JPEG (v2) so it stays inspectable after the full-frame JPEG expires;
// regions without a stored crop (v1 rows) fall back to regenerating the crop from the frame while it is retained.
//
// Independent of Argus. Standard library + the other libvisualmemory headers only.
#ifndef VISUALMEMORY_OBJECT_INDEX_H
#define VISUALMEMORY_OBJECT_INDEX_H

#include "visualmemory/embedding_provider.h"
#include "visualmemory/object_frame_source.h"
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
    // Crop JPEGs (v2). Independent of the full-frame retention. max_crop_bytes == 0 disables the quota.
    bool        store_crops = true;
    int         crop_max_side = 256;             // longest side of the stored crop JPEG (never upscaled)
    int         crop_quality = 80;
    uint64_t    max_crop_bytes = 256ull << 20;
};

// How an automatically indexed region was produced. Absent (nullptr) for manual regions.
struct DetectionInfo {
    int         class_id = -1;
    std::string class_name;
    float       confidence = 0.0f;
    std::string detector;                        // e.g. "yolox_s"
    std::string detector_version;
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
    // ---- schema v2 ----
    std::string index_origin = "manual";         // "manual" (user selected) | "auto" (detector)
    int         class_id = -1;                   // detector class, -1 for manual regions
    std::string class_name;                      // detector class name, empty for manual regions
    float       confidence = -1.0f;              // detector score, < 0 when there is none (manual)
    std::string detector, detector_version;
    std::string crop_state = "none";             // "retained" | "expired" (removed by retention) | "none" (never stored, v1 row)
    uint64_t    crop_bytes = 0;
    int         crop_width = 0, crop_height = 0;
    std::string crop_rel;                        // internal: crop file path relative to the storage root (never expose it)
    // ---- schema v3: where the parent frame came from ----
    std::string source_type = "camera";          // "camera" | "display"
    std::string source_kind;                     // "" (camera) | "history" | "checkpoint" (display)
    std::string pop_session_id, expected_media_id, reported_media_id, provenance;   // display only
    int64_t     playback_offset_ms = -1;         // display only: offset into the player-reported play
    int         time_uncertainty_ms = -1;        // display only: -1 = no stated bound
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
    std::string source_type;                     // "camera" | "display", empty = all sources (the API defaults to camera)
    std::string index_origin;                    // "manual" | "auto", empty = both
    std::string class_name;                      // detector class (exact), empty = any
    bool        has_min_similarity = false;
    float       min_similarity = 0.0f;
    bool        require_image = false;           // drop hits with neither a retained crop nor a retained parent JPEG
    int64_t     exclude_region_id = 0;
};

struct ObjectHit {
    ObjectRegion region;
    float similarity = 0.0f, distance = 0.0f;    // cosine; NOT a calibrated identification probability
    bool  image_available = false;               // parent frame JPEG still retained (DB flag)
    bool  image_exists = false;                  // file present right now
    bool  crop_available = false;                // the region's own crop JPEG is retained and present
};

struct ObjectSearchResult {
    SearchStatus status;                         // code Ok with hits empty => empty index / no match
    std::vector<ObjectHit> hits;
    EmbeddingSpec space;                         // embedding space the scores belong to
    uint64_t index_size = 0;                     // regions in that space
};

struct ObjectFilter { int64_t start_utc_ms = 0, end_utc_ms = 0; std::string source_id, label, index_origin, class_name, source_type, source_kind, pop_session_id; int64_t frame_id = 0; };

// Per-frame result of the automatic indexer (persisted: prevents duplicate work and lets a restart resume).
struct FrameProgress {
    std::string source_type = "camera", source_kind;     // identity of the frame together with frame_id
    int64_t     frame_id = 0;
    std::string detector, detector_version, source_id;
    int64_t     capture_utc_ms = 0;
    std::string status;                          // "ok" | "image_unavailable" | "error"
    int         objects_found = 0, objects_indexed = 0;
    float       threshold = 0.0f;
    std::string error;
    int64_t     processed_utc_ms = 0;
};

struct CropPruneResult { uint64_t removed = 0, bytes_freed = 0; };

struct ObjectIndexSummary {
    bool open = false;
    int  schema_version = 0;
    uint64_t regions = 0, vector_bytes = 0, db_bytes = 0;
    int64_t oldest_capture_utc_ms = 0, newest_capture_utc_ms = 0;
    std::vector<EmbeddingSpec> models;
    // ---- schema v2 ----
    uint64_t manual_regions = 0, auto_regions = 0;
    std::vector<std::pair<std::string, uint64_t>> by_class;      // auto regions per class name
    uint64_t crops_retained = 0, crops_expired = 0, crop_bytes = 0;
    uint64_t frames_processed = 0, frames_with_objects = 0, frames_failed = 0;
    int64_t  oldest_processed_capture_utc_ms = 0, newest_processed_capture_utc_ms = 0, last_processed_utc_ms = 0;
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
    // `det` (optional) marks the region as automatically indexed and stores its detector provenance; the label then
    // defaults to the class name. The crop JPEG is stored per ObjectIndexConfig (best effort: a failed crop write
    // does not fail the indexing, crop_state stays "none").
    ObjectIndexResult index_region(ObjectFrameSource& src, EmbeddingProvider& provider, const std::string& kind, int64_t frame_id,
                                   const Roi& roi, const std::string& label, const DetectionInfo* det = nullptr);
    ObjectIndexResult index_region(VisualMemoryReader& frames, EmbeddingProvider& provider, int64_t frame_id,        // camera frame
                                   const Roi& roi, const std::string& label, const DetectionInfo* det = nullptr);

    // One frame's automatic result in ONE short transaction (rows + vectors + the frame-progress record + an optional meta
    // key/value such as the live cursor): a single commit per frame instead of one per object plus bookkeeping, so readers
    // of objects.db (dashboard) wait at most one commit, not several. All heavy work (JPEG decode, crop, embedding, crop
    // file writes) happens BEFORE the transaction. A region already indexed for this frame/box/model is left untouched
    // (never relabelled) and counted as a duplicate. On any failure nothing is written (new crop files are removed).
    struct FrameRegion { Roi roi; DetectionInfo det; };
    struct FrameResult {
        ObjectIndexResult::Code code = ObjectIndexResult::Code::NotOpen;
        std::string message;
        int indexed = 0, duplicates = 0, invalid = 0;
        double elapsed_ms = 0;
        double embed_ms = 0; int embed_count = 0;       // MobileNetV2 time summed over the crops of this frame, and their number (measurement only)
        bool ok() const { return code == ObjectIndexResult::Code::Indexed; }
    };
    FrameResult index_frame(ObjectFrameSource& src, EmbeddingProvider& provider, const std::string& kind, int64_t frame_id,
                            const std::vector<FrameRegion>& regions, FrameProgress progress,
                            const std::string& meta_key = {}, const std::string& meta_value = {});
    FrameResult index_frame(VisualMemoryReader& frames, EmbeddingProvider& provider, int64_t frame_id,              // camera frame
                            const std::vector<FrameRegion>& regions, FrameProgress progress,
                            const std::string& meta_key = {}, const std::string& meta_value = {});

    // Stored crop JPEG of a region. 0 ok, 1 unknown region, 2 no crop was stored / removed by retention
    // (`err` says which), 3 crop file missing or unreadable.
    int stored_crop_jpeg(int64_t region_id, std::vector<uint8_t>& out, std::string& err);

    // ---- automatic-indexing progress (persisted in objects.db) ----
    bool get_frame_progress(const std::string& source_type, const std::string& kind, int64_t frame_id, const std::string& detector,
                            const std::string& version, FrameProgress& out);
    bool get_frame_progress(int64_t frame_id, const std::string& detector, const std::string& version, FrameProgress& out);   // camera frame
    bool record_frame_progress(const FrameProgress& p);            // upsert
    bool get_meta(const std::string& key, std::string& value);
    bool set_meta(const std::string& key, const std::string& value);

    // Crop-image retention (independent of region/embedding retention). Removes the crop FILES of crops created
    // before `older_than_created_ms` (0 = skip) and then, while the retained crop bytes exceed `max_bytes`
    // (0 = no quota), the oldest crops. Region rows, embeddings, boxes and timestamps are kept: crop_state becomes "expired".
    CropPruneResult prune_crops(int64_t older_than_created_ms, uint64_t max_bytes);

    // Crop of a stored frame as JPEG (regenerated; the stored frame is untouched). Return codes as
    // VisualMemoryReader::read_jpeg (0 ok, 1 unknown frame, 2 expired, 3 unreadable) plus 4 invalid ROI, 5 decode.
    int crop_jpeg(ObjectFrameSource& src, const std::string& kind, int64_t frame_id, const Roi& roi, int max_side, int quality,
                  std::vector<uint8_t>& out, std::string& err);
    int crop_jpeg(VisualMemoryReader& frames, int64_t frame_id, const Roi& roi, int max_side, int quality,            // camera frame
                  std::vector<uint8_t>& out, std::string& err);
    int region_crop_jpeg(const ObjectSources& src, int64_t region_id, int max_side, int quality,
                         std::vector<uint8_t>& out, std::string& err);
    int region_crop_jpeg(VisualMemoryReader& frames, int64_t region_id, int max_side, int quality,
                         std::vector<uint8_t>& out, std::string& err);

    // Searches (object vectors only). `frames` (optional) is used to report image availability.
    // `src` / `avail`: the sources a hit's parent frame is resolved against (display hits against the display source, camera hits
    // against the camera source). Which objects are searched is chosen by ObjectSearchOptions::source_type, never by id.
    ObjectSearchResult search_by_roi(ObjectFrameSource& src, const ObjectSources& avail, EmbeddingProvider& provider, const std::string& kind,
                                     int64_t frame_id, const Roi& roi, const ObjectSearchOptions& o);
    ObjectSearchResult search_by_roi(VisualMemoryReader& frames, EmbeddingProvider& provider, int64_t frame_id,       // camera frame
                                     const Roi& roi, const ObjectSearchOptions& o);
    ObjectSearchResult search_by_jpeg(const ObjectSources& avail, EmbeddingProvider& provider,
                                      const std::vector<uint8_t>& jpeg, const ObjectSearchOptions& o);
    ObjectSearchResult search_by_jpeg(VisualMemoryReader* frames, EmbeddingProvider& provider,
                                      const std::vector<uint8_t>& jpeg, const ObjectSearchOptions& o);
    ObjectSearchResult search_by_region(const ObjectSources& avail, int64_t region_id, const ObjectSearchOptions& o);
    ObjectSearchResult search_by_region(VisualMemoryReader* frames, int64_t region_id, const ObjectSearchOptions& o);
    ObjectSearchResult search_by_vector(const ObjectSources& avail, const std::vector<float>& v,
                                        const EmbeddingSpec& space, const ObjectSearchOptions& o);
    ObjectSearchResult search_by_vector(VisualMemoryReader* frames, const std::vector<float>& v,
                                        const EmbeddingSpec& space, const ObjectSearchOptions& o);

    bool get_region(int64_t region_id, ObjectRegion& out);
    std::vector<ObjectRegion> list_regions(const ObjectFilter& f, int limit, int offset, bool newest_first,
                                           uint64_t* total = nullptr);
    bool set_label(int64_t region_id, const std::string& label);
    bool delete_region(int64_t region_id);
    // source_type "" = every source; "camera" / "display" restrict every count (regions, classes, crops, processed frames) to it.
    ObjectIndexSummary summary(const std::string& source_type = std::string());

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
