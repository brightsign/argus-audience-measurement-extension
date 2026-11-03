// SPDX-License-Identifier: MIT
#include "retention.h"

#include "file_store.h"
#include "sqlite_store.h"

namespace visualmemory::detail {

namespace {
constexpr int kBatch = 256;  // delete in bounded batches to cap memory/latency
}

RetentionResult run_retention(SqliteStore& db, FileStore& files,
                              int64_t now_utc_ms, int retention_hours,
                              uint64_t max_storage_bytes) {
    RetentionResult res;

    // 1) Age-based expiry.
    if (retention_hours > 0) {
        const int64_t cutoff =
            now_utc_ms - static_cast<int64_t>(retention_hours) * 3600 * 1000;
        for (;;) {
            auto batch = db.select_older_than(cutoff, kBatch);
            if (batch.empty()) break;
            for (const auto& c : batch) {
                files.remove(c.relative_jpeg_path);  // missing file tolerated
                if (db.delete_by_id(c.id)) {
                    ++res.deleted;
                    res.freed_bytes += c.jpeg_size_bytes;
                }
            }
            if (batch.size() < static_cast<size_t>(kBatch)) break;
        }
    }

    // 2) Quota-based eviction (oldest first).
    if (max_storage_bytes > 0) {
        while (db.total_jpeg_bytes() > max_storage_bytes) {
            auto batch = db.select_oldest(kBatch);
            if (batch.empty()) break;
            for (const auto& c : batch) {
                files.remove(c.relative_jpeg_path);
                if (db.delete_by_id(c.id)) {
                    ++res.deleted;
                    res.freed_bytes += c.jpeg_size_bytes;
                }
                if (db.total_jpeg_bytes() <= max_storage_bytes) break;
            }
        }
    }
    return res;
}

} // namespace visualmemory::detail
