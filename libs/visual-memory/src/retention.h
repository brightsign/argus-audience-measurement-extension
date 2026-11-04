// SPDX-License-Identifier: MIT
// Internal: age- and quota-based retention over the managed store.
#ifndef VISUALMEMORY_RETENTION_H
#define VISUALMEMORY_RETENTION_H

#include <cstdint>

namespace visualmemory::detail {

class SqliteStore;
class FileStore;
class VectorStore;

struct RetentionResult {
    uint64_t deleted = 0;
    uint64_t freed_bytes = 0;
};

// Delete frames older than retention_hours, then evict oldest frames until the
// logical JPEG total is within max_storage_bytes. Missing files are tolerated:
// the metadata row is still removed to keep DB and disk consistent.
//
// Stage 2: when `vs` is non-null, JPEG expiry/eviction no longer deletes the metadata row of
// a frame that still has an embedding — the row is kept with image_available=0 so the
// embedding stays searchable. Embeddings expire independently after `embedding_retention_days`
// (0 = keep until quota), and the quota covers JPEG bytes + vector bytes. With vs == nullptr the
// behaviour is exactly Stage 1.
RetentionResult run_retention(SqliteStore& db, FileStore& files,
                              int64_t now_utc_ms,
                              int retention_hours,
                              uint64_t max_storage_bytes,
                              VectorStore* vs = nullptr,
                              int embedding_retention_days = 0);

} // namespace visualmemory::detail

#endif // VISUALMEMORY_RETENTION_H
