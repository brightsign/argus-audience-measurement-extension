// SPDX-License-Identifier: MIT
#include "vector_store.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>

namespace visualmemory::detail {
namespace {

int64_t now_utc_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    explicit Stmt(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) s = nullptr;
    }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
};

bool exec(sqlite3* db, const char* sql, std::string* err = nullptr) {
    char* msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        if (err) *err = msg ? msg : sqlite3_errmsg(db);
        sqlite3_free(msg);
        return false;
    }
    return true;
}

std::string vec_table(int64_t id)  { return "vec_emb_" + std::to_string(id); }
std::string blob_table(int64_t id) { return "blob_emb_" + std::to_string(id); }
std::string data_table(const ModelInfo& m) {
    return m.backend == "vec0" ? vec_table(m.model_id) : blob_table(m.model_id);
}
std::string text(sqlite3_stmt* s, int c) {
    const unsigned char* t = sqlite3_column_text(s, c);
    return t ? reinterpret_cast<const char*>(t) : std::string{};
}

bool create_data_table(sqlite3* db, const ModelInfo& m, std::string& err) {
    std::string sql;
    if (m.backend == "vec0") {
        // chunk_size=128 (sqlite-vec default 1024): every new chunk allocates a zeroblob of
        // chunk_size*dim*4 bytes (5.2 MB for 1280-d at 1024; 655 kB at 128). On the SD card the big
        // allocation stalled the inserting transaction for seconds (measured on XT5) and KNN reads one
        // chunk at a time, so a smaller chunk also bounds query working memory.
        sql = "CREATE VIRTUAL TABLE IF NOT EXISTS " + vec_table(m.model_id) +
              " USING vec0(embedding float[" + std::to_string(m.spec.dimension) +
              "] distance_metric=cosine, capture_ms integer, source_ix integer, chunk_size=128);";
    } else {
        sql = "CREATE TABLE IF NOT EXISTS " + blob_table(m.model_id) +
              " (frame_id INTEGER PRIMARY KEY, embedding BLOB NOT NULL, capture_ms INTEGER NOT NULL,"
              " source_ix INTEGER NOT NULL);"
              "CREATE INDEX IF NOT EXISTS idx_" + blob_table(m.model_id) + "_t ON " +
              blob_table(m.model_id) + "(capture_ms);";
    }
    return exec(db, sql.c_str(), &err);
}

bool row_to_model(sqlite3_stmt* st, ModelInfo& m) {
    m.model_id = sqlite3_column_int64(st, 0);
    m.spec.model_name = text(st, 1);
    m.spec.model_version = text(st, 2);
    m.spec.model_sha256 = text(st, 3);
    m.spec.preprocessing = text(st, 4);
    m.spec.dimension = sqlite3_column_int(st, 5);
    m.spec.dtype = text(st, 6);
    m.spec.metric = text(st, 7);
    m.backend = text(st, 8);
    return true;
}
const char* kModelCols =
    "model_id,model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric,backend";

} // namespace

int64_t VectorStore::source_ix(sqlite3* db, const std::string& source_id, bool create) {
    {
        Stmt q(db, "SELECT ix FROM embedding_sources WHERE source_id=?;");
        if (!q) return -1;
        sqlite3_bind_text(q.s, 1, source_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q.s) == SQLITE_ROW) return sqlite3_column_int64(q.s, 0);
    }
    if (!create) return -1;
    Stmt ins(db, "INSERT OR IGNORE INTO embedding_sources(source_id) VALUES(?);");
    if (!ins) return -1;
    sqlite3_bind_text(ins.s, 1, source_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(ins.s) != SQLITE_DONE) return -1;
    Stmt q2(db, "SELECT ix FROM embedding_sources WHERE source_id=?;");
    if (!q2) return -1;
    sqlite3_bind_text(q2.s, 1, source_id.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(q2.s) == SQLITE_ROW ? sqlite3_column_int64(q2.s, 0) : -1;
}

bool VectorStore::register_model(const EmbeddingSpec& spec, bool prefer_vec, ModelInfo& out,
                                 std::string& err) {
    if (spec.dimension <= 0 || spec.dimension > 8192) { err = "invalid embedding dimension"; return false; }
    if (spec.dtype != "f32") { err = "unsupported vector dtype '" + spec.dtype + "' (only f32 stored)"; return false; }
    if (spec.metric != "cosine") { err = "unsupported metric '" + spec.metric + "'"; return false; }
    const bool use_vec = prefer_vec && db_.vec_available();
    return db_.with_db([&](sqlite3* db) -> bool {
        if (!db) { err = "database closed"; return false; }
        if (!exec(db, "BEGIN IMMEDIATE;", &err)) return false;
        auto rollback = [&]() { exec(db, "ROLLBACK;"); return false; };
        ModelInfo m;
        {
            Stmt q(db, std::string("SELECT ") + kModelCols +
                       " FROM embedding_models WHERE model_name=? AND model_version=? AND model_sha256=?"
                       " AND preprocessing=? AND dimension=? AND dtype=? AND metric=?;");
            if (!q) { err = sqlite3_errmsg(db); return rollback(); }
            sqlite3_bind_text(q.s, 1, spec.model_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q.s, 2, spec.model_version.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q.s, 3, spec.model_sha256.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q.s, 4, spec.preprocessing.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(q.s, 5, spec.dimension);
            sqlite3_bind_text(q.s, 6, spec.dtype.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q.s, 7, spec.metric.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(q.s) == SQLITE_ROW) row_to_model(q.s, m);
        }
        if (m.valid()) {
            // Existing model keeps its backend: switching it would orphan stored vectors.
            if (m.backend == "vec0" && !db_.vec_available()) {
                err = "model was stored with sqlite-vec but sqlite-vec is unavailable in this build";
                return rollback();
            }
        } else {
            Stmt ins(db, "INSERT INTO embedding_models(model_name,model_version,model_sha256,preprocessing,"
                         "dimension,dtype,metric,backend,created_utc_ms) VALUES(?,?,?,?,?,?,?,?,?);");
            if (!ins) { err = sqlite3_errmsg(db); return rollback(); }
            sqlite3_bind_text(ins.s, 1, spec.model_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins.s, 2, spec.model_version.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins.s, 3, spec.model_sha256.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins.s, 4, spec.preprocessing.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins.s, 5, spec.dimension);
            sqlite3_bind_text(ins.s, 6, spec.dtype.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins.s, 7, spec.metric.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins.s, 8, use_vec ? "vec0" : "blob", -1, SQLITE_STATIC);
            sqlite3_bind_int64(ins.s, 9, now_utc_ms());
            if (sqlite3_step(ins.s) != SQLITE_DONE) { err = sqlite3_errmsg(db); return rollback(); }
            m.model_id = sqlite3_last_insert_rowid(db);
            m.spec = spec;
            m.backend = use_vec ? "vec0" : "blob";
        }
        if (!create_data_table(db, m, err)) return rollback();
        if (!exec(db, "COMMIT;", &err)) return rollback();
        out = m;
        return true;
    });
}

InsertResult VectorStore::insert(const ModelInfo& m, int64_t frame_id, const float* v,
                                 int64_t capture_ms, const std::string& source_id, std::string& err) {
    return db_.with_db([&](sqlite3* db) -> InsertResult {
        if (!db) { err = "database closed"; return InsertResult::Error; }
        if (!exec(db, "BEGIN IMMEDIATE;", &err)) return InsertResult::Error;
        auto fail = [&](const char* what) {
            err = std::string(what) + ": " + sqlite3_errmsg(db);
            exec(db, "ROLLBACK;");
            return InsertResult::Error;
        };
        {   // idempotence: any existing row (ok or failed) for this pair
            Stmt q(db, "SELECT status FROM frame_embeddings WHERE frame_id=? AND model_id=?;");
            if (!q) return fail("prepare exists");
            sqlite3_bind_int64(q.s, 1, frame_id);
            sqlite3_bind_int64(q.s, 2, m.model_id);
            if (sqlite3_step(q.s) == SQLITE_ROW) {
                const int status = sqlite3_column_int(q.s, 0);
                if (status == 1) { exec(db, "ROLLBACK;"); return InsertResult::AlreadyExists; }
                // previously failed: replace the failure marker with the vector
                Stmt del(db, "DELETE FROM frame_embeddings WHERE frame_id=? AND model_id=?;");
                if (!del) return fail("prepare del failed-marker");
                sqlite3_bind_int64(del.s, 1, frame_id);
                sqlite3_bind_int64(del.s, 2, m.model_id);
                if (sqlite3_step(del.s) != SQLITE_DONE) return fail("del failed-marker");
            }
        }
        const int64_t six = source_ix(db, source_id, true);
        if (six < 0) return fail("source_ix");
        const size_t bytes = static_cast<size_t>(m.spec.dimension) * sizeof(float);
        if (m.backend == "vec0") {
            Stmt ins(db, "INSERT INTO " + vec_table(m.model_id) +
                         "(rowid,embedding,capture_ms,source_ix) VALUES(?,?,?,?);");
            if (!ins) return fail("prepare vec insert");
            sqlite3_bind_int64(ins.s, 1, frame_id);
            sqlite3_bind_blob(ins.s, 2, v, static_cast<int>(bytes), SQLITE_STATIC);
            sqlite3_bind_int64(ins.s, 3, capture_ms);
            sqlite3_bind_int64(ins.s, 4, six);
            if (sqlite3_step(ins.s) != SQLITE_DONE) return fail("vec insert");
        } else {
            Stmt ins(db, "INSERT OR REPLACE INTO " + blob_table(m.model_id) +
                         "(frame_id,embedding,capture_ms,source_ix) VALUES(?,?,?,?);");
            if (!ins) return fail("prepare blob insert");
            sqlite3_bind_int64(ins.s, 1, frame_id);
            sqlite3_bind_blob(ins.s, 2, v, static_cast<int>(bytes), SQLITE_STATIC);
            sqlite3_bind_int64(ins.s, 3, capture_ms);
            sqlite3_bind_int64(ins.s, 4, six);
            if (sqlite3_step(ins.s) != SQLITE_DONE) return fail("blob insert");
        }
        {
            Stmt fe(db, "INSERT INTO frame_embeddings(frame_id,model_id,status,created_utc_ms,error)"
                        " VALUES(?,?,1,?,NULL);");
            if (!fe) return fail("prepare frame_embeddings");
            sqlite3_bind_int64(fe.s, 1, frame_id);
            sqlite3_bind_int64(fe.s, 2, m.model_id);
            sqlite3_bind_int64(fe.s, 3, now_utc_ms());
            if (sqlite3_step(fe.s) != SQLITE_DONE) return fail("frame_embeddings insert");
        }
        if (!exec(db, "COMMIT;", &err)) { exec(db, "ROLLBACK;"); return InsertResult::Error; }
        return InsertResult::Inserted;
    });
}

bool VectorStore::mark_failed(const ModelInfo& m, int64_t frame_id, const std::string& why) {
    return db_.with_db([&](sqlite3* db) -> bool {
        if (!db) return false;
        Stmt st(db, "INSERT OR IGNORE INTO frame_embeddings(frame_id,model_id,status,created_utc_ms,error)"
                    " VALUES(?,?,2,?,?);");
        if (!st) return false;
        sqlite3_bind_int64(st.s, 1, frame_id);
        sqlite3_bind_int64(st.s, 2, m.model_id);
        sqlite3_bind_int64(st.s, 3, now_utc_ms());
        sqlite3_bind_text(st.s, 4, why.c_str(), -1, SQLITE_TRANSIENT);
        return sqlite3_step(st.s) == SQLITE_DONE;
    });
}

bool VectorStore::has_ok_embedding(int64_t frame_id) {
    return db_.with_db([&](sqlite3* db) -> bool {
        if (!db) return false;
        Stmt q(db, "SELECT 1 FROM frame_embeddings WHERE frame_id=? AND status=1 LIMIT 1;");
        if (!q) return false;
        sqlite3_bind_int64(q.s, 1, frame_id);
        return sqlite3_step(q.s) == SQLITE_ROW;
    });
}

std::vector<PendingFrame> VectorStore::frames_missing(const ModelInfo& m, int64_t min_capture_ms,
                                                      int64_t before_capture_ms, int limit) {
    std::vector<PendingFrame> out;
    db_.with_db([&](sqlite3* db) -> int {
        if (!db) return 0;
        Stmt q(db, "SELECT f.id,f.relative_jpeg_path,f.source_id,f.capture_utc_ms FROM frames f "
                   "WHERE f.image_available=1 AND f.capture_utc_ms>=? AND f.capture_utc_ms<? "
                   "AND NOT EXISTS (SELECT 1 FROM frame_embeddings e WHERE e.frame_id=f.id AND e.model_id=?) "
                   "ORDER BY f.capture_utc_ms DESC, f.id DESC LIMIT ?;");
        if (!q) return 0;
        sqlite3_bind_int64(q.s, 1, min_capture_ms);
        sqlite3_bind_int64(q.s, 2, before_capture_ms);
        sqlite3_bind_int64(q.s, 3, m.model_id);
        sqlite3_bind_int(q.s, 4, limit);
        while (sqlite3_step(q.s) == SQLITE_ROW) {
            PendingFrame p;
            p.id = sqlite3_column_int64(q.s, 0);
            p.relative_path = text(q.s, 1);
            p.source_id = text(q.s, 2);
            p.capture_utc_ms = sqlite3_column_int64(q.s, 3);
            out.push_back(std::move(p));
        }
        return 0;
    });
    return out;
}

bool VectorStore::knn(const ModelInfo& m, const float* q, int k, int64_t start_ms, int64_t end_ms,
                      const std::string& source_id, std::vector<KnnHit>& out, uint64_t* scanned,
                      std::string& err) {
    out.clear();
    if (scanned) *scanned = 0;
    const size_t dim = static_cast<size_t>(m.spec.dimension);
    return db_.with_db([&](sqlite3* db) -> bool {
        if (!db) { err = "database closed"; return false; }
        int64_t six = -1;
        if (!source_id.empty()) {
            six = source_ix(db, source_id, false);
            if (six < 0) return true;           // unknown source => no candidates
        }
        const bool has_start = start_ms > 0, has_end = end_ms > 0;
        if (m.backend == "vec0") {
            std::string sql = "SELECT rowid,distance FROM " + vec_table(m.model_id) +
                              " WHERE embedding MATCH ? AND k = ?";
            if (has_start) sql += " AND capture_ms >= ?";
            if (has_end)   sql += " AND capture_ms < ?";
            if (six >= 0)  sql += " AND source_ix = ?";
            sql += " ORDER BY distance;";
            Stmt st(db, sql);
            if (!st) { err = sqlite3_errmsg(db); return false; }
            int i = 1;
            sqlite3_bind_blob(st.s, i++, q, static_cast<int>(dim * sizeof(float)), SQLITE_STATIC);
            sqlite3_bind_int(st.s, i++, k);
            if (has_start) sqlite3_bind_int64(st.s, i++, start_ms);
            if (has_end)   sqlite3_bind_int64(st.s, i++, end_ms);
            if (six >= 0)  sqlite3_bind_int64(st.s, i++, six);
            int rc;
            while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) {
                KnnHit h; h.frame_id = sqlite3_column_int64(st.s, 0);
                h.distance = static_cast<float>(sqlite3_column_double(st.s, 1));
                out.push_back(h);
            }
            if (rc != SQLITE_DONE) { err = sqlite3_errmsg(db); return false; }
            if (scanned) {   // vec0 scans every row passing the metadata filter; report that count
                std::string csql = "SELECT COUNT(*) FROM " + vec_table(m.model_id) + " WHERE 1";
                if (has_start) csql += " AND capture_ms >= ?";
                if (has_end)   csql += " AND capture_ms < ?";
                if (six >= 0)  csql += " AND source_ix = ?";
                Stmt cs(db, csql);
                if (cs) {
                    int j = 1;
                    if (has_start) sqlite3_bind_int64(cs.s, j++, start_ms);
                    if (has_end)   sqlite3_bind_int64(cs.s, j++, end_ms);
                    if (six >= 0)  sqlite3_bind_int64(cs.s, j++, six);
                    if (sqlite3_step(cs.s) == SQLITE_ROW) *scanned = static_cast<uint64_t>(sqlite3_column_int64(cs.s, 0));
                }
            }
            return true;
        }
        // ---- BLOB fallback: streaming exact cosine with a bounded top-k heap. ----
        std::string sql = "SELECT frame_id,embedding FROM " + blob_table(m.model_id) + " WHERE 1";
        if (has_start) sql += " AND capture_ms >= ?";
        if (has_end)   sql += " AND capture_ms < ?";
        if (six >= 0)  sql += " AND source_ix = ?";
        Stmt st(db, sql);
        if (!st) { err = sqlite3_errmsg(db); return false; }
        int i = 1;
        if (has_start) sqlite3_bind_int64(st.s, i++, start_ms);
        if (has_end)   sqlite3_bind_int64(st.s, i++, end_ms);
        if (six >= 0)  sqlite3_bind_int64(st.s, i++, six);
        auto cmp = [](const KnnHit& a, const KnnHit& b) { return a.distance < b.distance; };  // max-heap
        std::priority_queue<KnnHit, std::vector<KnnHit>, decltype(cmp)> heap(cmp);
        uint64_t n = 0;
        int rc;
        while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) {
            ++n;
            const void* b = sqlite3_column_blob(st.s, 1);
            if (!b || static_cast<size_t>(sqlite3_column_bytes(st.s, 1)) != dim * sizeof(float)) continue;
            const float* e = static_cast<const float*>(b);
            double dot = 0;
            for (size_t d = 0; d < dim; ++d) dot += static_cast<double>(q[d]) * e[d];
            KnnHit h; h.frame_id = sqlite3_column_int64(st.s, 0); h.distance = static_cast<float>(1.0 - dot);
            if (static_cast<int>(heap.size()) < k) heap.push(h);
            else if (h.distance < heap.top().distance) { heap.pop(); heap.push(h); }
        }
        if (rc != SQLITE_DONE) { err = sqlite3_errmsg(db); return false; }
        if (scanned) *scanned = n;
        while (!heap.empty()) { out.push_back(heap.top()); heap.pop(); }
        std::sort(out.begin(), out.end(), [](const KnnHit& a, const KnnHit& b) {
            return a.distance != b.distance ? a.distance < b.distance : a.frame_id < b.frame_id; });
        return true;
    });
}

uint64_t VectorStore::count(const ModelInfo& m) {
    return db_.with_db([&](sqlite3* db) -> uint64_t {
        if (!db) return 0;
        Stmt q(db, "SELECT COUNT(*) FROM frame_embeddings WHERE model_id=? AND status=1;");
        if (!q) return 0;
        sqlite3_bind_int64(q.s, 1, m.model_id);
        return sqlite3_step(q.s) == SQLITE_ROW ? static_cast<uint64_t>(sqlite3_column_int64(q.s, 0)) : 0;
    });
}

uint64_t VectorStore::total_vector_bytes() {
    return db_.with_db([&](sqlite3* db) -> uint64_t {
        if (!db) return 0;
        Stmt q(db, "SELECT COALESCE(SUM(m.dimension*4),0) FROM frame_embeddings e "
                   "JOIN embedding_models m ON m.model_id=e.model_id WHERE e.status=1;");
        if (!q) return 0;
        return sqlite3_step(q.s) == SQLITE_ROW ? static_cast<uint64_t>(sqlite3_column_int64(q.s, 0)) : 0;
    });
}

std::vector<ModelInfo> VectorStore::models() {
    std::vector<ModelInfo> out;
    db_.with_db([&](sqlite3* db) -> int {
        if (!db) return 0;
        Stmt q(db, std::string("SELECT ") + kModelCols + " FROM embedding_models ORDER BY model_id;");
        if (!q) return 0;
        while (sqlite3_step(q.s) == SQLITE_ROW) { ModelInfo m; row_to_model(q.s, m); out.push_back(m); }
        return 0;
    });
    return out;
}

// Delete (frame_id, model_id) pairs: vector row -> frame_embeddings row -> frames row if the
// image is already gone and no other embedding references it. One transaction.
uint64_t VectorStore::delete_frames_embeddings(sqlite3* db,
                                               const std::vector<std::pair<int64_t, int64_t>>& fm) {
    if (fm.empty()) return 0;
    std::string e;
    if (!exec(db, "BEGIN IMMEDIATE;", &e)) return 0;
    uint64_t removed = 0;
    bool ok = true;
    std::vector<ModelInfo> ms;
    {
        Stmt q(db, std::string("SELECT ") + kModelCols + " FROM embedding_models;");
        if (q) while (sqlite3_step(q.s) == SQLITE_ROW) { ModelInfo m; row_to_model(q.s, m); ms.push_back(m); }
    }
    for (const auto& [fid, mid] : fm) {
        const ModelInfo* mi = nullptr;
        for (const auto& m : ms) if (m.model_id == mid) mi = &m;
        if (mi) {
            const std::string key = (mi->backend == "vec0") ? "rowid" : "frame_id";
            Stmt dv(db, "DELETE FROM " + data_table(*mi) + " WHERE " + key + "=?;");
            if (!dv) { ok = false; break; }
            sqlite3_bind_int64(dv.s, 1, fid);
            if (sqlite3_step(dv.s) != SQLITE_DONE) { ok = false; break; }
        }
        Stmt de(db, "DELETE FROM frame_embeddings WHERE frame_id=? AND model_id=?;");
        if (!de) { ok = false; break; }
        sqlite3_bind_int64(de.s, 1, fid);
        sqlite3_bind_int64(de.s, 2, mid);
        if (sqlite3_step(de.s) != SQLITE_DONE) { ok = false; break; }
        ++removed;
        Stmt df(db, "DELETE FROM frames WHERE id=? AND image_available=0 AND NOT EXISTS "
                    "(SELECT 1 FROM frame_embeddings WHERE frame_id=frames.id AND status=1);");
        if (!df) { ok = false; break; }
        sqlite3_bind_int64(df.s, 1, fid);
        if (sqlite3_step(df.s) != SQLITE_DONE) { ok = false; break; }
    }
    if (!ok) { exec(db, "ROLLBACK;"); return 0; }
    if (!exec(db, "COMMIT;", &e)) { exec(db, "ROLLBACK;"); return 0; }
    return removed;
}

uint64_t VectorStore::delete_older_than(int64_t cutoff_capture_ms, int limit) {
    return db_.with_db([&](sqlite3* db) -> uint64_t {
        if (!db) return 0;
        std::vector<std::pair<int64_t, int64_t>> fm;
        {
            Stmt q(db, "SELECT e.frame_id,e.model_id FROM frame_embeddings e JOIN frames f ON f.id=e.frame_id "
                       "WHERE f.capture_utc_ms<? ORDER BY f.capture_utc_ms,f.id LIMIT ?;");
            if (!q) return 0;
            sqlite3_bind_int64(q.s, 1, cutoff_capture_ms);
            sqlite3_bind_int(q.s, 2, limit);
            while (sqlite3_step(q.s) == SQLITE_ROW)
                fm.emplace_back(sqlite3_column_int64(q.s, 0), sqlite3_column_int64(q.s, 1));
        }
        return delete_frames_embeddings(db, fm);
    });
}

uint64_t VectorStore::delete_oldest(int limit) {
    return db_.with_db([&](sqlite3* db) -> uint64_t {
        if (!db) return 0;
        std::vector<std::pair<int64_t, int64_t>> fm;
        {
            Stmt q(db, "SELECT e.frame_id,e.model_id FROM frame_embeddings e JOIN frames f ON f.id=e.frame_id "
                       "ORDER BY f.capture_utc_ms,f.id LIMIT ?;");
            if (!q) return 0;
            sqlite3_bind_int(q.s, 1, limit);
            while (sqlite3_step(q.s) == SQLITE_ROW)
                fm.emplace_back(sqlite3_column_int64(q.s, 0), sqlite3_column_int64(q.s, 1));
        }
        return delete_frames_embeddings(db, fm);
    });
}

uint64_t VectorStore::reconcile(const ModelInfo& m) {
    return db_.with_db([&](sqlite3* db) -> uint64_t {
        if (!db) return 0;
        const std::string key = (m.backend == "vec0") ? "rowid" : "frame_id";
        const std::string dt = data_table(m);
        // Fingerprint (count, sum of ids) on both sides: cheap O(N) scans, no per-row lookups. Equal
        // fingerprints => consistent in practice; a mismatch triggers the exact per-row repair below.
        int64_t nvec = 0, nrow = 0, svec = 0, srow = 0;
        {
            Stmt a(db, "SELECT COUNT(*), COALESCE(SUM(" + key + "),0) FROM " + dt + ";");
            if (a && sqlite3_step(a.s) == SQLITE_ROW) { nvec = sqlite3_column_int64(a.s, 0); svec = sqlite3_column_int64(a.s, 1); }
            Stmt b(db, "SELECT COUNT(*), COALESCE(SUM(frame_id),0) FROM frame_embeddings WHERE model_id=? AND status=1;");
            if (b) { sqlite3_bind_int64(b.s, 1, m.model_id);
                     if (sqlite3_step(b.s) == SQLITE_ROW) { nrow = sqlite3_column_int64(b.s, 0); srow = sqlite3_column_int64(b.s, 1); } }
        }
        if (nvec == nrow && svec == srow) return 0;
        uint64_t repaired = 0;
        std::string e;
        if (!exec(db, "BEGIN IMMEDIATE;", &e)) return 0;
        {   // ok rows whose vector vanished -> drop the marker (frame gets re-embedded)
            Stmt q(db, "SELECT frame_id FROM frame_embeddings WHERE model_id=? AND status=1;");
            std::vector<int64_t> drop;
            if (q) {
                sqlite3_bind_int64(q.s, 1, m.model_id);
                Stmt ex(db, "SELECT 1 FROM " + dt + " WHERE " + key + "=?;");
                while (ex && sqlite3_step(q.s) == SQLITE_ROW) {
                    const int64_t fid = sqlite3_column_int64(q.s, 0);
                    sqlite3_reset(ex.s); sqlite3_bind_int64(ex.s, 1, fid);
                    if (sqlite3_step(ex.s) != SQLITE_ROW) drop.push_back(fid);
                }
            }
            for (int64_t fid : drop) {
                Stmt d(db, "DELETE FROM frame_embeddings WHERE frame_id=? AND model_id=?;");
                if (d) { sqlite3_bind_int64(d.s, 1, fid); sqlite3_bind_int64(d.s, 2, m.model_id);
                         if (sqlite3_step(d.s) == SQLITE_DONE) ++repaired; }
            }
        }
        {   // vectors with no ok marker -> delete the vector
            std::vector<int64_t> orphans;
            Stmt q(db, "SELECT " + key + " FROM " + dt + ";");
            Stmt ex(db, "SELECT 1 FROM frame_embeddings WHERE frame_id=? AND model_id=? AND status=1;");
            while (q && ex && sqlite3_step(q.s) == SQLITE_ROW) {
                const int64_t fid = sqlite3_column_int64(q.s, 0);
                sqlite3_reset(ex.s); sqlite3_bind_int64(ex.s, 1, fid); sqlite3_bind_int64(ex.s, 2, m.model_id);
                if (sqlite3_step(ex.s) != SQLITE_ROW) orphans.push_back(fid);
            }
            for (int64_t fid : orphans) {
                Stmt d(db, "DELETE FROM " + dt + " WHERE " + key + "=?;");
                if (d) { sqlite3_bind_int64(d.s, 1, fid);
                         if (sqlite3_step(d.s) == SQLITE_DONE) ++repaired; }
            }
        }
        if (!exec(db, "COMMIT;", &e)) { exec(db, "ROLLBACK;"); return 0; }
        return repaired;
    });
}

} // namespace visualmemory::detail
