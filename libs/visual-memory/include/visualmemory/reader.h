// SPDX-License-Identifier: MIT
// VisualMemoryReader: read-only access to a Visual Memory store from a SECOND process (e.g. a dashboard
// API). It never writes the database or the frame directory, never migrates the schema, never runs
// retention and starts no worker threads. Searches run on the caller's thread.
//
// Independent of Argus. Safe to use while the writer process (VisualMemory) is running: SQLite's own
// locking applies and a busy writer only delays a reader (bounded by busy_timeout_ms).
#ifndef VISUALMEMORY_READER_H
#define VISUALMEMORY_READER_H

#include "visualmemory/embedding_provider.h"
#include "visualmemory/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace visualmemory {

struct ReaderConfig {
    std::string root_directory;                 // same directory the writer uses (contains visual_memory.db, frames/)
    int         busy_timeout_ms = 3000;         // wait this long for a writer's transaction
    size_t      max_jpeg_bytes  = 8u * 1024 * 1024;
};

struct FrameFilter {
    int64_t     start_utc_ms = 0;               // inclusive, 0 = unbounded
    int64_t     end_utc_ms   = 0;               // exclusive, 0 = unbounded
    std::string source_id;                      // empty = all
};

struct EmbeddingRecord {
    int64_t     model_id = 0;
    std::string model_name, model_version, model_sha256, preprocessing;
    int         dimension = 0;
    bool        ok = false;                     // true: vector stored; false: permanent failure marker
    int64_t     created_utc_ms = 0;
    std::string error;
};

struct FrameRecord {
    FrameInfo   frame;                          // frame.relative_jpeg_path is internal: do not expose it
    bool        image_exists = false;           // file present right now (only meaningful if image_available)
    bool        has_embedding = false;          // at least one ok vector
};

struct FrameDetail {
    FrameRecord                  record;
    std::vector<EmbeddingRecord> embeddings;
};

struct ModelSummary {
    EmbeddingSpec spec;
    int64_t       model_id = 0;
    std::string   backend;                      // "vec0" | "blob"
    uint64_t      vectors_ok = 0, vectors_failed = 0;
};

struct StoreSummary {
    bool        open = false;
    int         schema_version = 0;
    std::string sqlite_version, vec_version;    // vec_version empty if sqlite-vec is not registered
    uint64_t    frames = 0, frames_with_image = 0;
    uint64_t    embeddings_ok = 0, embeddings_failed = 0;
    uint64_t    jpeg_bytes = 0, vector_bytes = 0, db_bytes = 0;
    int64_t     oldest_capture_utc_ms = 0, newest_capture_utc_ms = 0;     // 0 if empty
    int64_t     latest_embedding_created_utc_ms = 0;   // when the newest OK embedding was stored (0 if none)
    std::vector<std::string> sources;
    std::vector<ModelSummary> models;
};

class VisualMemoryReader {
public:
    explicit VisualMemoryReader(ReaderConfig cfg);
    ~VisualMemoryReader();
    VisualMemoryReader(const VisualMemoryReader&) = delete;
    VisualMemoryReader& operator=(const VisualMemoryReader&) = delete;

    // Opens <root>/visual_memory.db read-only. Fails (with err) if it is missing or its schema is older
    // than this library expects (the writer migrates, never the reader).
    bool open(std::string& err);
    bool is_open() const;
    void close();

    // Optional: an EmbeddingProvider enables search_by_jpeg / search_by_image (and search_by_frame_id for
    // frames that have no stored vector). The provider is only used for the QUERY image.
    void set_embedding_provider(std::shared_ptr<EmbeddingProvider> provider);

    StoreSummary summary();

    // Newest-first (or oldest-first) page. `total` (optional) receives the count matching the filter.
    std::vector<FrameRecord> list_frames(const FrameFilter& f, int limit, int offset, bool newest_first,
                                         uint64_t* total = nullptr);
    bool get_frame(int64_t id, FrameDetail& out);

    // Reads the stored JPEG of a frame (bounded). Returns:
    //   0 ok, 1 unknown id, 2 image expired (DB flag), 3 file missing/unreadable.
    int read_jpeg(int64_t id, std::vector<uint8_t>& out, std::string& err);

    // Same return codes as read_jpeg; produces a downscaled JPEG (width <= max_w, 32..1024).
    int read_thumbnail(int64_t id, int max_w, std::vector<uint8_t>& out, std::string& err);

    // Searches. `status` is always filled. The stored model is chosen as the one matching the provider's
    // spec (uploads) or the frame's own vector (search_by_frame_id).
    std::vector<SearchResult> search_by_jpeg(const std::vector<uint8_t>& jpeg, const SearchOptions& o,
                                             SearchStatus* status);
    std::vector<SearchResult> search_by_image(const ImageView& image, const SearchOptions& o,
                                              SearchStatus* status);
    // "Find frames similar to stored frame <id>". Uses its stored vector (no inference); if it has none and
    // its JPEG is readable and a provider is set, embeds the JPEG. The query frame is excluded.
    // `used_stored_vector` (optional) reports which path was taken.
    std::vector<SearchResult> search_by_frame_id(int64_t id, const SearchOptions& o, SearchStatus* status,
                                                 bool* used_stored_vector = nullptr);
    std::vector<SearchResult> search_similar(const std::vector<float>& embedding, const SearchOptions& o,
                                             SearchStatus* status, const EmbeddingSpec* query_space = nullptr);

    // Abort a search running on another thread (sqlite3_interrupt). The search returns Code::Cancelled.
    void cancel_search();

    // Absolute path resolution for tests/tools only; the dashboard API must not expose it.
    std::string absolute_path(const std::string& relative_jpeg_path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace visualmemory

#endif // VISUALMEMORY_READER_H
