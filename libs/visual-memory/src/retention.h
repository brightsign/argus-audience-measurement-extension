// SPDX-License-Identifier: MIT
// Internal: age- and quota-based retention over the managed store.
#ifndef VISUALMEMORY_RETENTION_H
#define VISUALMEMORY_RETENTION_H

#include <cstdint>

namespace visualmemory::detail {

class SqliteStore;
class FileStore;

struct RetentionResult {
    uint64_t deleted = 0;
    uint64_t freed_bytes = 0;
};

// Delete frames older than retention_hours, then evict oldest frames until the
// logical JPEG total is within max_storage_bytes. Missing files are tolerated:
// the metadata row is still removed to keep DB and disk consistent.
RetentionResult run_retention(SqliteStore& db, FileStore& files,
                              int64_t now_utc_ms,
                              int retention_hours,
                              uint64_t max_storage_bytes);

} // namespace visualmemory::detail

#endif // VISUALMEMORY_RETENTION_H
