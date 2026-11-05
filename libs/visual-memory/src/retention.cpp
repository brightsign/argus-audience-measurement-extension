// SPDX-License-Identifier: MIT
#include "retention.h"

#include "file_store.h"
#include "sqlite_store.h"
#include "vector_store.h"

#include <algorithm>
#include <chrono>

namespace visualmemory::detail {

namespace {
constexpr int kBatch = 256;  // delete in bounded batches to cap memory/latency

// Remove the JPEG. Rows that still own an embedding survive (image marked unavailable);
// everything else is deleted exactly as in Stage 1. Returns true if the JPEG was retired.
bool retire_jpeg(SqliteStore& db, FileStore& files, VectorStore* vs, const EvictCandidate& c) {
    files.remove(c.relative_jpeg_path);  // missing file tolerated
    if (vs && vs->has_ok_embedding(c.id)) return db.mark_image_unavailable(c.id);
    return db.delete_by_id(c.id);
}

uint64_t total_bytes(SqliteStore& db, VectorStore* vs) {
    return db.total_jpeg_bytes() + (vs ? vs->total_vector_bytes() : 0);
}
}

RetentionResult run_retention(SqliteStore& db, FileStore& files,
                              int64_t now_utc_ms, int retention_hours,
                              uint64_t max_storage_bytes,
                              VectorStore* vs, int embedding_retention_days,
                              const RetentionBudget& budget) {
    RetentionResult res;
    const auto t0 = std::chrono::steady_clock::now();
    // True once the slice budget is spent. Checked between single deletions, so one slice never
    // runs longer than max_ms plus one file removal + row delete.
    // At least one item is always processed per call, so a slice can never be all overhead (progress guarantee).
    uint64_t attempted = 0;
    auto spent = [&]() {
        if (attempted == 0) return false;
        if (budget.max_deletions > 0 && res.deleted >= budget.max_deletions) return true;
        if (budget.max_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= budget.max_ms)
            return true;
        return false;
    };

    // 1) Age-based JPEG expiry.
    if (retention_hours > 0) {
        const int64_t cutoff =
            now_utc_ms - static_cast<int64_t>(retention_hours) * 3600 * 1000;
        for (;;) {
            auto batch = db.select_older_than(cutoff, kBatch);   // only image_available=1 rows
            if (batch.empty()) break;
            for (const auto& c : batch) {
                if (spent()) { res.more = true; return res; }
                ++attempted;
                if (retire_jpeg(db, files, vs, c)) {
                    ++res.deleted;
                    res.freed_bytes += c.jpeg_size_bytes;
                }
            }
            if (batch.size() < static_cast<size_t>(kBatch)) break;
        }
    }

    // 2) Independent embedding expiry (one cheap SQL batch per call; checked against the budget).
    if (vs && embedding_retention_days > 0) {
        const int64_t cutoff = now_utc_ms - static_cast<int64_t>(embedding_retention_days) * 86400LL * 1000;
        while (vs->delete_older_than(cutoff, kBatch) == static_cast<uint64_t>(kBatch)) {
            ++attempted;
            if (spent()) { res.more = true; return res; }
        }
    }

    // 3) Quota (JPEG + vectors): evict oldest JPEGs first, then oldest embeddings.
    if (max_storage_bytes > 0) {
        while (total_bytes(db, vs) > max_storage_bytes) {
            auto batch = db.select_oldest(kBatch);
            if (batch.empty()) {
                if (!vs || vs->delete_oldest(kBatch) == 0) break;   // nothing left to evict
                continue;
            }
            for (const auto& c : batch) {
                if (spent()) { res.more = true; return res; }
                ++attempted;
                if (retire_jpeg(db, files, vs, c)) {
                    ++res.deleted;
                    res.freed_bytes += c.jpeg_size_bytes;
                }
                if (total_bytes(db, vs) <= max_storage_bytes) break;
            }
        }
    }
    return res;
}

} // namespace visualmemory::detail
