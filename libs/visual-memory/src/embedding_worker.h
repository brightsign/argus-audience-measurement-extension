// SPDX-License-Identifier: MIT
// Internal: Stage 2 embedding pipeline. A separate bounded queue + worker thread so
// slow NPU inference or vector insertion can never block Stage 1 JPEG persistence
// or the Argus capture thread. Tasks carry only (frame id, relative path, ...) — no
// pixel buffers — so queue memory is ~100 B per entry.
#ifndef VISUALMEMORY_EMBEDDING_WORKER_H
#define VISUALMEMORY_EMBEDDING_WORKER_H

#include "file_store.h"
#include "sqlite_store.h"
#include "vector_store.h"
#include "visualmemory/embedding_provider.h"
#include "visualmemory/latency_ring.h"
#include "visualmemory/types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace visualmemory::detail {

class EmbeddingWorker {
public:
    EmbeddingWorker(SqliteStore& db, FileStore& files, VectorStore& vs,
                    std::shared_ptr<EmbeddingProvider> provider, EmbeddingOptions opt,
                    ModelInfo model, int64_t jpeg_retention_floor_ms);
    ~EmbeddingWorker();

    void start();
    void stop();

    // Non-blocking, bounded. Returns false (and counts a drop) if the queue is full.
    bool try_enqueue(int64_t frame_id, const std::string& rel_path, int64_t capture_ms,
                     const std::string& source_id);

    // Synchronous bounded backfill (also used by the standalone utility): embeds up to
    // `max_frames` stored JPEGs that have no vector for this model, newest first.
    // Idempotent and resumable (selection is "frames lacking a vector"). Returns the
    // number of vectors stored. `deadline_ms` > 0 bounds wall time.
    size_t backfill(size_t max_frames, int64_t deadline_ms, bool startup = false);

    // Embed + validate + L2-normalise a decoded image (shares the provider with the worker).
    bool embed_image(const ImageView& img, std::vector<float>& out, std::string& err);

    EmbeddingStats stats() const;
    const ModelInfo& model() const { return model_; }
    EmbeddingProvider& provider() { return *provider_; }
    void note_search(int64_t ns) { lat_search_.add(ns); searches_.fetch_add(1); }

    // Re-read vector count/bytes from SQLite into the cached atomics. stats() NEVER touches SQLite,
    // so it is safe to call from any thread, including Argus' capture thread (no DB-mutex waits).
    void refresh_counts();

private:
    struct Task { int64_t frame_id; std::string rel; int64_t capture_ms; std::string source; bool backfill; };
    enum class Outcome { Stored, Exists, PermanentFail, TransientFail };

    void loop();
    Outcome process(const Task& t);

    SqliteStore& db_; FileStore& files_; VectorStore& vs_;
    std::shared_ptr<EmbeddingProvider> provider_;
    EmbeddingOptions opt_;
    ModelInfo model_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Task> q_;
    std::atomic<bool> stop_{false}, running_{false}, backfill_done_{false};
    std::thread th_;

    std::atomic<uint64_t> enqueued_{0}, dropped_{0}, embedded_{0}, backfilled_{0}, retries_{0},
        backfill_attempted_{0}, infer_fail_{0}, invalid_{0}, decode_fail_{0}, perm_fail_{0}, db_fail_{0}, searches_{0};
    std::atomic<size_t> max_depth_{0};
    std::atomic<uint64_t> vectors_cached_{0}, vector_bytes_cached_{0};
    std::mutex counts_mu_;   // makes (DB insert + cache increment) atomic w.r.t. refresh_counts(); never taken by stats()
    LatencyRing lat_read_, lat_decode_, lat_insert_, lat_total_, lat_search_;
    std::mutex provider_mu_;   // serialises batch use of provider stats snapshots only
};

} // namespace visualmemory::detail

#endif // VISUALMEMORY_EMBEDDING_WORKER_H
