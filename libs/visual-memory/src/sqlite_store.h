// SPDX-License-Identifier: MIT
// Internal: SQLite metadata store. A single connection, serialised by an
// internal mutex, serves both the worker (inserts/retention) and query callers.
// SQLite headers stay confined to this translation unit.
#ifndef VISUALMEMORY_SQLITE_STORE_H
#define VISUALMEMORY_SQLITE_STORE_H

#include "visualmemory/types.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace visualmemory::detail {

struct InsertRow {
    std::string source_id;
    std::string source_type;
    std::string device_id;
    int64_t     capture_utc_ms = 0;
    int64_t     pts_ns = 0;
    uint64_t    source_sequence = 0;
    int         stored_width = 0;
    int         stored_height = 0;
    std::string pixel_format;
    std::string relative_jpeg_path;
    uint64_t    jpeg_size_bytes = 0;
    std::string jpeg_sha256;
    int64_t     created_utc_ms = 0;
};

// A row eligible for eviction (retention / quota).
struct EvictCandidate {
    int64_t     id = 0;
    std::string relative_jpeg_path;
    uint64_t    jpeg_size_bytes = 0;
    int64_t     capture_utc_ms = 0;
};

class SqliteStore {
public:
    SqliteStore() = default;
    ~SqliteStore();

    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    // Open (creating if needed), apply pragmas and run migrations.
    bool open(const std::string& db_path, std::string& err);
    void close();

    // Insert one row. Returns the new rowid, or -1 on failure.
    int64_t insert(const InsertRow& row);

    // Query capture_utc_ms in [start, end), optionally filtered by source_id,
    // ordered by (capture_utc_ms, id). limit<=0 means unlimited.
    std::vector<FrameInfo> query_by_time(int64_t start_utc_ms, int64_t end_utc_ms,
                                         const std::string& source_id,
                                         int limit, int offset);

    std::vector<EvictCandidate> select_oldest(int limit);
    std::vector<EvictCandidate> select_older_than(int64_t cutoff_utc_ms, int limit);
    bool    delete_by_id(int64_t id);

    // Stage 2: the JPEG expired but an embedding is kept — keep the metadata row,
    // flag the image unavailable so quota/retention no longer count or revisit it.
    bool    mark_image_unavailable(int64_t id);
    // One frame row (id = frames.id). Returns false if absent.
    bool    get_by_id(int64_t id, FrameInfo& out);

    uint64_t total_jpeg_bytes();     // only frames whose JPEG is still available
    int64_t  row_count();

    // Stage 2 support ------------------------------------------------------
    // True if sqlite-vec was compiled in and registered on this connection.
    bool        vec_available() const { return vec_available_; }
    std::string vec_version() const { return vec_version_; }
    std::string sqlite_version() const;
    int         schema_version() const { return schema_version_; }

    // Run fn(sqlite3*) while holding the store mutex (used by VectorStore so
    // every SQL statement on the single connection stays serialised).
    template <class F>
    auto with_db(F&& fn) {
        std::lock_guard<std::mutex> lk(m_);
        return fn(db_);
    }

private:
    bool exec(const char* sql, std::string& err);

    sqlite3*           db_ = nullptr;
    mutable std::mutex m_;
    bool               vec_available_ = false;
    std::string        vec_version_;
    int                schema_version_ = 0;
};

} // namespace visualmemory::detail

#endif // VISUALMEMORY_SQLITE_STORE_H
