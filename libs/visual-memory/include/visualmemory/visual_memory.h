// SPDX-License-Identifier: MIT
//
// libvisualmemory — public façade.
//
// Receives owned frames through a non-blocking bounded queue, asynchronously
// encodes JPEGs and records metadata in SQLite, and answers time/source
// queries. The capture thread never performs file, JPEG or SQL work.
//
// Depends only on the C++ standard library in its public surface.
#ifndef VISUALMEMORY_VISUAL_MEMORY_H
#define VISUALMEMORY_VISUAL_MEMORY_H

#include "visualmemory/embedding_provider.h"
#include "visualmemory/types.h"

#include <memory>
#include <string>
#include <vector>

namespace visualmemory {

class VisualMemory {
public:
    explicit VisualMemory(Config cfg);
    ~VisualMemory();

    VisualMemory(const VisualMemory&)            = delete;
    VisualMemory& operator=(const VisualMemory&) = delete;

    // Open storage, run migrations and launch the worker thread.
    // Returns false (and leaves the object inert) on invalid config or storage
    // initialisation failure — callers must keep running regardless.
    bool start();

    // Move an already-owned frame into the bounded queue. NEVER blocks on disk
    // or database I/O; only a brief mutex to push the pointer. Returns false if
    // the library is stopped, the queue is full, or the frame is invalid.
    bool try_submit(OwnedFrame&& frame);

    // Query persisted frames with capture_utc_ms in [start_utc_ms, end_utc_ms).
    // Results are ordered by (capture_utc_ms, id). An empty source_id matches
    // all sources. limit<=0 means "no limit".
    std::vector<FrameInfo> query_by_time(int64_t start_utc_ms,
                                         int64_t end_utc_ms,
                                         const std::string& source_id = {},
                                         int limit = 0,
                                         int offset = 0);

    // Resolve a FrameInfo::relative_jpeg_path to an absolute filesystem path.
    std::string absolute_path(const std::string& relative_jpeg_path) const;

    // ---- Stage 2 (optional; inert unless Config::embedding.enabled) -------------
    // Inject the embedding provider (RKNN, mock, ...). Must be called BEFORE start().
    // The library never owns device resources: the caller shuts the provider down.
    void set_embedding_provider(std::shared_ptr<EmbeddingProvider> provider);

    // True when a ready provider is registered and the embedding worker is running.
    bool embeddings_active() const;

    // Exact top-K cosine search over stored embeddings. `embedding` is validated
    // (dimension, NaN/Inf, zero norm) and L2-normalised internally. `query_space`
    // (optional) is checked against the active model identity so vectors from a
    // different model/preprocessing version are rejected (ModelMismatch). Results are
    // sorted by similarity (descending). Executes on the CALLER's thread; cost is
    // O(N*d) over the rows passing the time/source filter (see README "Scaling").
    std::vector<SearchResult> search_similar(const std::vector<float>& embedding,
                                             const SearchOptions& options,
                                             SearchStatus* status = nullptr,
                                             const EmbeddingSpec* query_space = nullptr);

    // Embed `image` (RGB24/BGR24) with the registered provider, then search.
    std::vector<SearchResult> search_by_image(const ImageView& image, const SearchOptions& options,
                                              SearchStatus* status = nullptr);
    // Decode a JPEG (bounded), embed, then search.
    std::vector<SearchResult> search_by_jpeg(const std::vector<uint8_t>& jpeg,
                                             const SearchOptions& options,
                                             SearchStatus* status = nullptr);

    // Bounded, resumable, idempotent backfill: embed up to `max_frames` stored JPEGs that
    // have no vector for the active model (newest first). Returns vectors stored.
    // `deadline_ms` > 0 caps wall time. Safe to call repeatedly or after a crash.
    size_t backfill_embeddings(size_t max_frames, int64_t deadline_ms = 0);

    EmbeddingStats embedding_stats() const;

    // Snapshot of counters.
    Stats stats() const;

    // Drain-then-stop: signals the worker, lets it finish the in-flight frame,
    // discards anything still queued, and joins. Safe to call multiple times.
    void stop();

    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace visualmemory

#endif // VISUALMEMORY_VISUAL_MEMORY_H
