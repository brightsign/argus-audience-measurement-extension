// SPDX-License-Identifier: MIT
#include "sqlite_store.h"

#include <sqlite3.h>

#include <cstring>

#ifdef VM_HAVE_SQLITE_VEC
extern "C" {
#include "sqlite-vec.h"
}
#endif

namespace visualmemory::detail {
namespace {

constexpr int kSchemaVersion = 2;

const char* kSchemaV1 = R"SQL(
CREATE TABLE IF NOT EXISTS frames (
    id                 INTEGER PRIMARY KEY,
    source_id          TEXT NOT NULL,
    source_type        TEXT NOT NULL,
    device_id          TEXT NOT NULL,
    capture_utc_ms     INTEGER NOT NULL,
    pts_ns             INTEGER,
    source_sequence    INTEGER,
    stored_width       INTEGER NOT NULL,
    stored_height      INTEGER NOT NULL,
    pixel_format       TEXT NOT NULL,
    relative_jpeg_path TEXT NOT NULL UNIQUE,
    jpeg_size_bytes    INTEGER NOT NULL,
    jpeg_sha256        TEXT,
    created_utc_ms     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_frames_time ON frames(capture_utc_ms);
CREATE INDEX IF NOT EXISTS idx_frames_source_time ON frames(source_id, capture_utc_ms);
)SQL";

// v2 (Stage 2): additive only. `frames` keeps every v1 column; image_available is
// added by ALTER (guarded in code so the migration is idempotent), and the embedding
// registry tables are new. Per-model vector tables (vec_emb_<id> / blob_emb_<id>) are
// created lazily when a model registers.
const char* kSchemaV2 = R"SQL(
CREATE TABLE IF NOT EXISTS embedding_models (
    model_id       INTEGER PRIMARY KEY,
    model_name     TEXT NOT NULL,
    model_version  TEXT NOT NULL,
    model_sha256   TEXT NOT NULL DEFAULT '',
    preprocessing  TEXT NOT NULL,
    dimension      INTEGER NOT NULL,
    dtype          TEXT NOT NULL,
    metric         TEXT NOT NULL,
    backend        TEXT NOT NULL,
    created_utc_ms INTEGER NOT NULL,
    UNIQUE(model_name, model_version, model_sha256, preprocessing, dimension, dtype, metric)
);
CREATE TABLE IF NOT EXISTS embedding_sources (
    ix        INTEGER PRIMARY KEY,
    source_id TEXT NOT NULL UNIQUE
);
CREATE TABLE IF NOT EXISTS frame_embeddings (
    frame_id       INTEGER NOT NULL REFERENCES frames(id) ON DELETE CASCADE,
    model_id       INTEGER NOT NULL REFERENCES embedding_models(model_id),
    status         INTEGER NOT NULL,
    created_utc_ms INTEGER NOT NULL,
    error          TEXT,
    PRIMARY KEY (frame_id, model_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_frame_embeddings_model ON frame_embeddings(model_id, status);
)SQL";

} // namespace

SqliteStore::~SqliteStore() { close(); }

bool SqliteStore::exec(const char* sql, std::string& err) {
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        err = msg ? msg : "sqlite exec failed";
        sqlite3_free(msg);
        return false;
    }
    return true;
}

bool SqliteStore::open(const std::string& db_path, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        err = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        return false;
    }
    // Conservative durability defaults. WAL is intentionally NOT forced: the
    // target SD-card filesystem locking/power-loss behaviour must be verified
    // before enabling it. DELETE journalling + FULL sync is the safe baseline.
    std::string e;
    if (!exec("PRAGMA journal_mode=DELETE;", e)) { err = e; return false; }
    if (!exec("PRAGMA synchronous=FULL;", e)) { err = e; return false; }
    if (!exec("PRAGMA foreign_keys=ON;", e)) { err = e; return false; }
    sqlite3_busy_timeout(db_, 5000);

#ifdef VM_HAVE_SQLITE_VEC
    // Explicit static registration — no runtime extension loading.
    {
        char* vec_err = nullptr;
        if (sqlite3_vec_init(db_, &vec_err, nullptr) == SQLITE_OK) {
            vec_available_ = true;
            sqlite3_stmt* vs = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT vec_version();", -1, &vs, nullptr) == SQLITE_OK) {
                if (sqlite3_step(vs) == SQLITE_ROW && sqlite3_column_text(vs, 0))
                    vec_version_ = reinterpret_cast<const char*>(sqlite3_column_text(vs, 0));
                sqlite3_finalize(vs);
            }
        }
        sqlite3_free(vec_err);   // vec failure is non-fatal: callers fall back to BLOB search
    }
#endif

    // Schema migration via PRAGMA user_version.
    int version = 0;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &st, nullptr) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) version = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
    }
    if (version < 1) {
        if (!exec(kSchemaV1, e)) { err = e; return false; }
    }
    // (user_version is advanced by the migration steps below, never lowered.)
    if (version < 2) {
        // Idempotent v1 -> v2: a Stage 1 binary may have reset user_version on a v2
        // file, so inspect the real schema instead of trusting the number.
        bool has_col = false;
        sqlite3_stmt* ti = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA table_info(frames);", -1, &ti, nullptr) == SQLITE_OK) {
            while (sqlite3_step(ti) == SQLITE_ROW) {
                const unsigned char* n = sqlite3_column_text(ti, 1);
                if (n && std::strcmp(reinterpret_cast<const char*>(n), "image_available") == 0) has_col = true;
            }
            sqlite3_finalize(ti);
        }
        if (!exec("BEGIN IMMEDIATE;", e)) { err = e; return false; }
        bool ok = true;
        if (!has_col) ok = exec("ALTER TABLE frames ADD COLUMN image_available INTEGER NOT NULL DEFAULT 1;", e);
        if (ok) ok = exec(kSchemaV2, e);
        if (ok) ok = exec("PRAGMA user_version=2;", e);   // allowed inside a transaction
        if (!ok) { std::string e2; exec("ROLLBACK;", e2); err = e; return false; }
        if (!exec("COMMIT;", e)) { err = e; return false; }
        version = 2;
    }
    schema_version_ = version > kSchemaVersion ? version : kSchemaVersion;
    return true;
}

bool SqliteStore::open_readonly(const std::string& db_path, int busy_timeout_ms, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    // Read-only: no pragmas that write, no migration, no journal creation. A writer (the Argus
    // process) owns schema upgrades; an older schema is reported, never modified.
    if (sqlite3_open_v2(db_path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        err = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(db_, busy_timeout_ms);
#ifdef VM_HAVE_SQLITE_VEC
    {
        char* vec_err = nullptr;
        if (sqlite3_vec_init(db_, &vec_err, nullptr) == SQLITE_OK) {
            vec_available_ = true;
            sqlite3_stmt* vs = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT vec_version();", -1, &vs, nullptr) == SQLITE_OK) {
                if (sqlite3_step(vs) == SQLITE_ROW && sqlite3_column_text(vs, 0))
                    vec_version_ = reinterpret_cast<const char*>(sqlite3_column_text(vs, 0));
                sqlite3_finalize(vs);
            }
        }
        sqlite3_free(vec_err);
    }
#endif
    int version = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) version = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    } else { err = sqlite3_errmsg(db_); sqlite3_close(db_); db_ = nullptr; return false; }
    if (version < kSchemaVersion) {
        err = "schema version " + std::to_string(version) + " < " + std::to_string(kSchemaVersion) +
              " (the writer must migrate it first)";
        sqlite3_close(db_); db_ = nullptr;
        return false;
    }
    schema_version_ = version;
    return true;
}

void SqliteStore::interrupt() {
    // Deliberately NOT under m_: sqlite3_interrupt is thread-safe and must be able to abort a
    // statement running on another thread while that thread holds m_.
    sqlite3* d = db_;
    if (d) sqlite3_interrupt(d);
}

void SqliteStore::close() {
    std::lock_guard<std::mutex> lk(m_);
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

int64_t SqliteStore::insert(const InsertRow& r) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return -1;
    static const char* sql =
        "INSERT INTO frames(source_id,source_type,device_id,capture_utc_ms,pts_ns,"
        "source_sequence,stored_width,stored_height,pixel_format,relative_jpeg_path,"
        "jpeg_size_bytes,jpeg_sha256,created_utc_ms) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, r.source_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, r.source_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, r.device_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, r.capture_utc_ms);
    sqlite3_bind_int64(st, 5, r.pts_ns);
    sqlite3_bind_int64(st, 6, static_cast<int64_t>(r.source_sequence));
    sqlite3_bind_int(st, 7, r.stored_width);
    sqlite3_bind_int(st, 8, r.stored_height);
    sqlite3_bind_text(st, 9, r.pixel_format.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, r.relative_jpeg_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 11, static_cast<int64_t>(r.jpeg_size_bytes));
    if (r.jpeg_sha256.empty())
        sqlite3_bind_null(st, 12);
    else
        sqlite3_bind_text(st, 12, r.jpeg_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 13, r.created_utc_ms);

    int64_t id = -1;
    if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(st);
    return id;
}

std::vector<FrameInfo> SqliteStore::query_by_time(int64_t start_utc_ms, int64_t end_utc_ms,
                                                  const std::string& source_id,
                                                  int limit, int offset) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<FrameInfo> out;
    if (!db_) return out;

    std::string sql =
        "SELECT id,source_id,source_type,device_id,capture_utc_ms,pts_ns,source_sequence,"
        "stored_width,stored_height,relative_jpeg_path,jpeg_size_bytes,jpeg_sha256,created_utc_ms,image_available "
        "FROM frames WHERE capture_utc_ms>=? AND capture_utc_ms<?";
    if (!source_id.empty()) sql += " AND source_id=?";
    sql += " ORDER BY capture_utc_ms,id";
    if (limit > 0) sql += " LIMIT ? OFFSET ?";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    int idx = 1;
    sqlite3_bind_int64(st, idx++, start_utc_ms);
    sqlite3_bind_int64(st, idx++, end_utc_ms);
    if (!source_id.empty())
        sqlite3_bind_text(st, idx++, source_id.c_str(), -1, SQLITE_TRANSIENT);
    if (limit > 0) {
        sqlite3_bind_int(st, idx++, limit);
        sqlite3_bind_int(st, idx++, offset > 0 ? offset : 0);
    }

    auto col_text = [](sqlite3_stmt* s, int c) -> std::string {
        const unsigned char* t = sqlite3_column_text(s, c);
        return t ? reinterpret_cast<const char*>(t) : std::string{};
    };
    while (sqlite3_step(st) == SQLITE_ROW) {
        FrameInfo fi;
        fi.id = sqlite3_column_int64(st, 0);
        fi.metadata.source_id = col_text(st, 1);
        fi.metadata.source_type = col_text(st, 2);
        fi.metadata.device_id = col_text(st, 3);
        fi.metadata.capture_utc_ms = sqlite3_column_int64(st, 4);
        fi.metadata.pts_ns = sqlite3_column_int64(st, 5);
        fi.metadata.source_sequence = static_cast<uint64_t>(sqlite3_column_int64(st, 6));
        fi.width = sqlite3_column_int(st, 7);
        fi.height = sqlite3_column_int(st, 8);
        fi.relative_jpeg_path = col_text(st, 9);
        fi.file_size_bytes = static_cast<uint64_t>(sqlite3_column_int64(st, 10));
        fi.sha256_hex = col_text(st, 11);
        fi.created_utc_ms = sqlite3_column_int64(st, 12);
        fi.image_available = sqlite3_column_int(st, 13) != 0;
        out.push_back(std::move(fi));
    }
    sqlite3_finalize(st);
    return out;
}

static std::vector<EvictCandidate> select_candidates(sqlite3* db, const char* sql,
                                                     int64_t bind_value, bool has_bind,
                                                     int limit) {
    std::vector<EvictCandidate> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    int idx = 1;
    if (has_bind) sqlite3_bind_int64(st, idx++, bind_value);
    sqlite3_bind_int(st, idx++, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        EvictCandidate c;
        c.id = sqlite3_column_int64(st, 0);
        const unsigned char* t = sqlite3_column_text(st, 1);
        c.relative_jpeg_path = t ? reinterpret_cast<const char*>(t) : std::string{};
        c.jpeg_size_bytes = static_cast<uint64_t>(sqlite3_column_int64(st, 2));
        c.capture_utc_ms = sqlite3_column_int64(st, 3);
        out.push_back(std::move(c));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<EvictCandidate> SqliteStore::select_oldest(int limit) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return {};
    return select_candidates(
        db_,
        "SELECT id,relative_jpeg_path,jpeg_size_bytes,capture_utc_ms FROM frames "
        "WHERE image_available=1 ORDER BY capture_utc_ms,id LIMIT ?;",
        0, false, limit);
}

std::vector<EvictCandidate> SqliteStore::select_older_than(int64_t cutoff_utc_ms, int limit) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return {};
    return select_candidates(
        db_,
        "SELECT id,relative_jpeg_path,jpeg_size_bytes,capture_utc_ms FROM frames "
        "WHERE capture_utc_ms<? AND image_available=1 ORDER BY capture_utc_ms,id LIMIT ?;",
        cutoff_utc_ms, true, limit);
}

bool SqliteStore::delete_by_id(int64_t id) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM frames WHERE id=?;", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bool ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
}

bool SqliteStore::mark_image_unavailable(int64_t id) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE frames SET image_available=0 WHERE id=?;", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bool ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
}

bool SqliteStore::get_by_id(int64_t id, FrameInfo& fi) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id,source_id,source_type,device_id,capture_utc_ms,pts_ns,source_sequence,"
            "stored_width,stored_height,relative_jpeg_path,jpeg_size_bytes,jpeg_sha256,"
            "created_utc_ms,image_available FROM frames WHERE id=?;", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        auto col_text = [&](int c) -> std::string {
            const unsigned char* t = sqlite3_column_text(st, c);
            return t ? reinterpret_cast<const char*>(t) : std::string{};
        };
        fi.id = sqlite3_column_int64(st, 0);
        fi.metadata.source_id = col_text(1);
        fi.metadata.source_type = col_text(2);
        fi.metadata.device_id = col_text(3);
        fi.metadata.capture_utc_ms = sqlite3_column_int64(st, 4);
        fi.metadata.pts_ns = sqlite3_column_int64(st, 5);
        fi.metadata.source_sequence = static_cast<uint64_t>(sqlite3_column_int64(st, 6));
        fi.width = sqlite3_column_int(st, 7);
        fi.height = sqlite3_column_int(st, 8);
        fi.relative_jpeg_path = col_text(9);
        fi.file_size_bytes = static_cast<uint64_t>(sqlite3_column_int64(st, 10));
        fi.sha256_hex = col_text(11);
        fi.created_utc_ms = sqlite3_column_int64(st, 12);
        fi.image_available = sqlite3_column_int(st, 13) != 0;
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

std::string SqliteStore::sqlite_version() const { return sqlite3_libversion(); }

uint64_t SqliteStore::total_jpeg_bytes() {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    uint64_t total = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COALESCE(SUM(jpeg_size_bytes),0) FROM frames WHERE image_available=1;", -1,
                           &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            total = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
    }
    return total;
}

int64_t SqliteStore::row_count() {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    int64_t n = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM frames;", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

} // namespace visualmemory::detail
