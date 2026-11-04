// SPDX-License-Identifier: MIT
// Internal: embedding registry + vector storage + top-K search on the shared
// SQLite connection (Stage 2).
//
// Data model
//   embedding_models   one row per distinct embedding space (name, version, sha256,
//                      preprocessing, dimension, dtype, metric). Search never mixes
//                      spaces: every model owns its own vector table.
//   frame_embeddings   (frame_id, model_id) -> status (1 ok / 2 failed) — the single
//                      source of truth for "does this frame have a vector".
//   vec_emb_<id>       sqlite-vec vec0 table, rowid = frames.id (stable observation id),
//                      metadata columns capture_ms + source_ix for pre-filtering.
//   blob_emb_<id>      fallback when sqlite-vec is unavailable: BLOB column scanned
//                      by a bounded C++ cosine loop.
//
// Neither backend is an ANN index: both are EXACT brute-force scans, O(N*d) per
// query (N = rows passing the metadata filter). See docs in libs/visual-memory/README.md.
#ifndef VISUALMEMORY_VECTOR_STORE_H
#define VISUALMEMORY_VECTOR_STORE_H

#include "sqlite_store.h"
#include "visualmemory/embedding_provider.h"

#include <cstdint>
#include <string>
#include <vector>

namespace visualmemory::detail {

struct ModelInfo {
    int64_t       model_id = 0;
    EmbeddingSpec spec;
    std::string   backend;           // "vec0" | "blob"
    bool valid() const { return model_id > 0; }
};

struct KnnHit {
    int64_t frame_id = 0;
    float   distance = 0;            // cosine distance (1 - cos) for L2-normalised vectors
};

struct PendingFrame {
    int64_t     id = 0;
    std::string relative_path;
    std::string source_id;
    int64_t     capture_utc_ms = 0;
};

enum class InsertResult { Inserted, AlreadyExists, Error };

class VectorStore {
public:
    explicit VectorStore(SqliteStore& db) : db_(db) {}

    // Register (or look up) the model and create its vector table. If the model
    // already exists, the stored dimension/backend must match.
    bool register_model(const EmbeddingSpec& spec, bool prefer_vec, ModelInfo& out, std::string& err);

    // Atomically (one transaction) record frame_embeddings + the vector. Idempotent:
    // a second insert for the same (frame, model) returns AlreadyExists and changes nothing.
    InsertResult insert(const ModelInfo& m, int64_t frame_id, const float* v,
                        int64_t capture_ms, const std::string& source_id, std::string& err);

    bool mark_failed(const ModelInfo& m, int64_t frame_id, const std::string& why);
    bool has_ok_embedding(int64_t frame_id);

    // Frames whose JPEG is available, newer than `min_capture_ms`, older than
    // `before_capture_ms` (exclusive; INT64_MAX = no bound) and without any
    // frame_embeddings row for this model. Newest first.
    std::vector<PendingFrame> frames_missing(const ModelInfo& m, int64_t min_capture_ms,
                                             int64_t before_capture_ms, int limit);

    // Exact top-K by cosine distance. Filters: capture time [start,end), source.
    // `scanned` (optional) receives the number of candidate rows the scan considered.
    bool knn(const ModelInfo& m, const float* q, int k, int64_t start_ms, int64_t end_ms,
             const std::string& source_id, std::vector<KnnHit>& out, uint64_t* scanned,
             std::string& err);

    // Read-side helpers (dashboard / reader).
    struct EmbeddingRow { int64_t model_id = 0; int status = 0; int64_t created_utc_ms = 0; std::string error; };
    std::vector<EmbeddingRow> frame_embeddings(int64_t frame_id);
    // The stored (already L2-normalised) vector of a frame for this model; false if none.
    bool get_vector(const ModelInfo& m, int64_t frame_id, std::vector<float>& out);
    uint64_t count_failed(const ModelInfo& m);   // status=2 rows of this model
    uint64_t count(const ModelInfo& m);        // status=1 rows of this model
    uint64_t total_vector_bytes();             // sum over all models (dimension*4 per ok row)
    std::vector<ModelInfo> models();

    // Embedding retention. Deletes vector + frame_embeddings rows for frames captured
    // before `cutoff_capture_ms`, then deletes the frame row itself if its JPEG is
    // already gone. Returns embeddings removed. `limit` bounds work per call.
    uint64_t delete_older_than(int64_t cutoff_capture_ms, int limit);
    // Quota path: drop the `limit` oldest embeddings (all models), same row cleanup.
    uint64_t delete_oldest(int limit);

    // Startup consistency check: remove frame_embeddings rows whose vector is missing
    // (so they are recomputed) and vectors without a frame_embeddings row. Returns repairs.
    uint64_t reconcile(const ModelInfo& m);

private:
    int64_t source_ix(sqlite3* db, const std::string& source_id, bool create);
    uint64_t delete_frames_embeddings(sqlite3* db, const std::vector<std::pair<int64_t,int64_t>>& frame_model);
    SqliteStore& db_;
};

} // namespace visualmemory::detail

#endif // VISUALMEMORY_VECTOR_STORE_H
