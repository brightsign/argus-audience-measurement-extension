// SPDX-License-Identifier: MIT
// D5: PopStore (pop.db) - reference library, playback sessions/events, display-capture evidence, verifications.
#include "visualmemory/pop.h"

#include "embedding_math.h"
#include "jpeg_decoder.h"
#include "sha256.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <tuple>
#include <set>
#include <unistd.h>

namespace fs = std::filesystem;

namespace visualmemory {

namespace {
constexpr int kSchemaVersion = 2;
constexpr size_t kMaxJpegBytes = 8u * 1024 * 1024;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string col_text(sqlite3_stmt* s, int c) {
    const unsigned char* t = sqlite3_column_text(s, c);
    return t ? reinterpret_cast<const char*>(t) : std::string{};
}
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const std::string& sql) { if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) s = nullptr; }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
    void text(int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), -1, SQLITE_TRANSIENT); }
};
bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id) if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    return id != "." && id != "..";
}
std::string two(int v) { char b[8]; std::snprintf(b, sizeof(b), "%02d", v); return b; }
std::string safe_segment(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (std::isalnum(c) || c == '.' || c == '_' || c == '-') ? static_cast<char>(c) : '_';
    return o.empty() ? "_" : o;
}
bool write_atomic(const fs::path& final_path, const std::vector<uint8_t>& bytes, std::string& err) {
    std::error_code ec;
    fs::create_directories(final_path.parent_path(), ec);
    if (ec) { err = "cannot create directory " + final_path.parent_path().string() + ": " + ec.message(); return false; }
    const fs::path tmp = fs::path(final_path.string() + ".tmp");
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { err = "cannot create " + tmp.string(); return false; }
    size_t w = 0;
    while (w < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + w, bytes.size() - w);
        if (n < 0) { ::close(fd); fs::remove(tmp, ec); err = "write failed"; return false; }
        w += static_cast<size_t>(n);
    }
    ::fsync(fd); ::close(fd);
    fs::rename(tmp, final_path, ec);
    if (ec) { fs::remove(tmp, ec); err = "rename failed: " + ec.message(); return false; }
    return true;
}
bool decode_embed(EmbeddingProvider& p, const std::vector<uint8_t>& jpeg, std::vector<float>& emb, int& w, int& h, std::string& err) {
    detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) return false;
    w = img.width; h = img.height;
    ImageView v; v.data = img.rgb.data(); v.width = img.width; v.height = img.height; v.stride = img.width * 3; v.format = PixelFormat::RGB24;
    if (!p.ready() || !p.embed(v, emb)) { err = "embedding failed: " + p.last_error(); return false; }
    const EmbeddingSpec sp = p.spec();
    if (detail::l2_normalize(emb, sp.dimension) != detail::VecCheck::Ok) { err = "embedding rejected (wrong dimension, NaN or zero norm)"; return false; }
    return true;
}
std::vector<float> blob_vec(sqlite3_stmt* s, int c) {
    const int n = sqlite3_column_bytes(s, c);
    std::vector<float> v(static_cast<size_t>(n) / sizeof(float));
    if (n > 0) std::memcpy(v.data(), sqlite3_column_blob(s, c), v.size() * sizeof(float));
    return v;
}
void bind_vec(sqlite3_stmt* s, int i, const std::vector<float>& v) {
    sqlite3_bind_blob(s, i, v.data(), static_cast<int>(v.size() * sizeof(float)), SQLITE_TRANSIENT);
}
int64_t as_i64(uint64_t h) { int64_t o; std::memcpy(&o, &h, 8); return o; }
uint64_t as_u64(int64_t h) { uint64_t o; std::memcpy(&o, &h, 8); return o; }

const char* kSessionCols = "session_id,job_id,expected_media_id,played_media_id,source,state,end_reason,scheduled_utc_ms,start_utc_ms,"
                           "end_utc_ms,actual_play_ms,slot_ms,captures_requested,created_at";
PopSession row_session(sqlite3_stmt* s) {
    PopSession p;
    p.session_id = col_text(s, 0); p.job_id = col_text(s, 1); p.expected_media_id = col_text(s, 2); p.played_media_id = col_text(s, 3);
    p.source = col_text(s, 4); p.state = col_text(s, 5); p.end_reason = col_text(s, 6);
    p.scheduled_utc_ms = sqlite3_column_int64(s, 7); p.start_utc_ms = sqlite3_column_int64(s, 8); p.end_utc_ms = sqlite3_column_int64(s, 9);
    p.actual_play_ms = sqlite3_column_int64(s, 10); p.slot_ms = sqlite3_column_int64(s, 11); p.captures_requested = sqlite3_column_int(s, 12);
    p.created_utc_ms = sqlite3_column_int64(s, 13);
    return p;
}
const char* kEvidenceCols = "evidence_id,session_id,idx,target_offset_ms,actual_offset_ms,status,failure_reason,source,path,phash,sha256,"
                            "width,height,bytes,took_ms,utc_ms,embedded,image_available,capture_method,provenance,uncertainty_ms";
// schema-1 (legacy) files have no capture-method columns: every row there is a screenshot by definition
const char* kEvidenceColsV1 = "evidence_id,session_id,idx,target_offset_ms,actual_offset_ms,status,failure_reason,source,path,phash,sha256,"
                              "width,height,bytes,took_ms,utc_ms,embedded,image_available,'brightscript_screenshot','',-1";
Evidence row_evidence(sqlite3_stmt* s) {
    Evidence e;
    e.evidence_id = sqlite3_column_int64(s, 0); e.session_id = col_text(s, 1); e.idx = sqlite3_column_int(s, 2);
    e.target_offset_ms = sqlite3_column_int(s, 3); e.actual_offset_ms = sqlite3_column_int(s, 4); e.status = col_text(s, 5);
    e.failure_reason = col_text(s, 6); e.source = col_text(s, 7); e.path = col_text(s, 8); e.phash = as_u64(sqlite3_column_int64(s, 9));
    e.sha256 = col_text(s, 10); e.width = sqlite3_column_int(s, 11); e.height = sqlite3_column_int(s, 12);
    e.bytes = static_cast<uint64_t>(sqlite3_column_int64(s, 13)); e.took_ms = sqlite3_column_int(s, 14); e.utc_ms = sqlite3_column_int64(s, 15);
    e.embedded = sqlite3_column_int(s, 16) != 0; e.image_available = sqlite3_column_int(s, 17) != 0;
    e.capture_method = col_text(s, 18); e.provenance = col_text(s, 19); e.uncertainty_ms = sqlite3_column_int(s, 20);
    return e;
}
const char* kHistCols = "frame_id,session_id,sec_idx,evidence_id,source,expected_media_id,played_media_id,utc_ms,mono_ms,target_offset_ms,actual_offset_ms,"
                        "uncertainty_ms,capture_method,provenance,path,sha256,width,height,bytes,embedded,image_available,embed_attempts,status";
HistoryFrame row_hist(sqlite3_stmt* s) {
    HistoryFrame h;
    h.frame_id = sqlite3_column_int64(s, 0); h.session_id = col_text(s, 1); h.sec_idx = sqlite3_column_int(s, 2); h.evidence_id = sqlite3_column_int64(s, 3);
    h.source = col_text(s, 4); h.expected_media_id = col_text(s, 5); h.played_media_id = col_text(s, 6); h.utc_ms = sqlite3_column_int64(s, 7);
    h.mono_ms = sqlite3_column_int64(s, 8); h.target_offset_ms = sqlite3_column_int(s, 9); h.actual_offset_ms = sqlite3_column_int(s, 10);
    h.uncertainty_ms = sqlite3_column_int(s, 11); h.capture_method = col_text(s, 12); h.provenance = col_text(s, 13); h.path = col_text(s, 14);
    h.sha256 = col_text(s, 15); h.width = sqlite3_column_int(s, 16); h.height = sqlite3_column_int(s, 17); h.bytes = static_cast<uint64_t>(sqlite3_column_int64(s, 18));
    h.embedded = sqlite3_column_int(s, 19) != 0; h.image_available = sqlite3_column_int(s, 20) != 0; h.embed_attempts = sqlite3_column_int(s, 21);
    h.status = col_text(s, 22); h.role = h.evidence_id ? "history_and_checkpoint" : "history_sample";
    return h;
}
// Dimensions from the SOF marker without decoding; false unless the buffer is a complete JPEG (SOI ... EOI) with a frame header.
bool jpeg_dims(const std::vector<uint8_t>& b, int& w, int& h) {
    if (b.size() < 128 || b[0] != 0xFF || b[1] != 0xD8 || b[b.size() - 2] != 0xFF || b[b.size() - 1] != 0xD9) return false;
    size_t i = 2;
    while (i + 9 < b.size()) {
        if (b[i] != 0xFF) { ++i; continue; }
        const uint8_t m = b[i + 1];
        if (m == 0xFF) { ++i; continue; }
        if (m == 0x00 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
        const size_t len = (static_cast<size_t>(b[i + 2]) << 8) | b[i + 3];
        if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            h = (b[i + 5] << 8) | b[i + 6]; w = (b[i + 7] << 8) | b[i + 8];
            return w > 0 && h > 0 && w <= 8192 && h <= 8192;
        }
        if (len < 2) return false;
        i += 2 + len;
    }
    return false;
}
std::string history_path(const std::string& source, int64_t utc_ms, const std::string& file_name) {
    std::time_t t = static_cast<std::time_t>(utc_ms / 1000);
    std::tm tmv{};
    gmtime_r(&t, &tmv);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "pop/history/%04d/%s/%s/%s/%s/%s", tmv.tm_year + 1900, two(tmv.tm_mon + 1).c_str(), two(tmv.tm_mday).c_str(),
                  two(tmv.tm_hour).c_str(), safe_segment(source).c_str(), safe_segment(file_name).c_str());
    return buf;
}
} // namespace

struct PopStore::Impl {
    PopConfig cfg;
    std::mutex mu;
    sqlite3* db = nullptr;
    bool v2 = true;                                   // false: legacy schema-1 file kept as is (no capture-method columns)
    bool hist = false;                                // display_history_frames exists (Phase 2); created only by history_open()
    explicit Impl(PopConfig c) : cfg(std::move(c)) {}

    bool exec(const char* sql, std::string& err) {
        char* m = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &m) != SQLITE_OK) { err = m ? m : "sqlite error"; sqlite3_free(m); return false; }
        return true;
    }
    int user_version() {
        Stmt q(db, "PRAGMA user_version;");
        return (q && sqlite3_step(q.s) == SQLITE_ROW) ? sqlite3_column_int(q.s, 0) : -1;
    }
    bool migrate(std::string& err) {
        const int v = user_version();
        if (v > kSchemaVersion) { err = "pop.db schema " + std::to_string(v) + " is newer than this library (" + std::to_string(kSchemaVersion) + ")"; return false; }
        if (v == 1 && !cfg.allow_schema_upgrade) { v2 = false; return true; }   // leave an existing database untouched
        if (v == 1) {                                  // v1 -> v2: record how each piece of evidence was captured
            if (!exec("BEGIN IMMEDIATE;", err)) return false;
            const char* up =
                "ALTER TABLE display_capture_evidence ADD COLUMN capture_method TEXT NOT NULL DEFAULT 'brightscript_screenshot';"
                "ALTER TABLE display_capture_evidence ADD COLUMN provenance TEXT NOT NULL DEFAULT '';"
                "ALTER TABLE display_capture_evidence ADD COLUMN uncertainty_ms INTEGER NOT NULL DEFAULT -1;"
                "PRAGMA user_version=2;";
            if (!exec(up, err)) { std::string e2; exec("ROLLBACK;", e2); return false; }
            return exec("COMMIT;", err);
        }
        if (v >= 2) return true;
        if (!exec("BEGIN IMMEDIATE;", err)) return false;
        const char* ddl =
            "CREATE TABLE IF NOT EXISTS pop_models(model_id INTEGER PRIMARY KEY,model_name TEXT NOT NULL,model_version TEXT NOT NULL,"
            " model_sha256 TEXT NOT NULL,preprocessing TEXT NOT NULL,dimension INTEGER NOT NULL,dtype TEXT NOT NULL,metric TEXT NOT NULL,"
            " UNIQUE(model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric));"
            "CREATE TABLE IF NOT EXISTS media_assets(media_id TEXT PRIMARY KEY,display_name TEXT NOT NULL,kind TEXT NOT NULL,uri TEXT NOT NULL,"
            " sha256 TEXT NOT NULL DEFAULT '',codec TEXT NOT NULL DEFAULT '',duration_s REAL NOT NULL DEFAULT 0,slot_s REAL NOT NULL DEFAULT 30,"
            " width INTEGER NOT NULL DEFAULT 0,height INTEGER NOT NULL DEFAULT 0,fps REAL NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS media_reference_frames(ref_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " media_id TEXT NOT NULL REFERENCES media_assets(media_id) ON DELETE CASCADE,offset_ms INTEGER NOT NULL,phash INTEGER NOT NULL,"
            " path TEXT NOT NULL,width INTEGER NOT NULL,height INTEGER NOT NULL,model_id INTEGER NOT NULL REFERENCES pop_models(model_id),"
            " embedding BLOB NOT NULL,created_at INTEGER NOT NULL,UNIQUE(media_id,offset_ms,model_id));"
            "CREATE TABLE IF NOT EXISTS playback_sessions(session_id TEXT PRIMARY KEY,job_id TEXT NOT NULL DEFAULT '',"
            " expected_media_id TEXT NOT NULL,played_media_id TEXT NOT NULL DEFAULT '',source TEXT NOT NULL DEFAULT 'main_display',"
            " state TEXT NOT NULL DEFAULT 'scheduled',end_reason TEXT NOT NULL DEFAULT '',scheduled_utc_ms INTEGER NOT NULL DEFAULT 0,"
            " start_utc_ms INTEGER NOT NULL DEFAULT 0,end_utc_ms INTEGER NOT NULL DEFAULT 0,actual_play_ms INTEGER NOT NULL DEFAULT -1,"
            " slot_ms INTEGER NOT NULL DEFAULT 30000,captures_requested INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS playback_events(event_id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,type TEXT NOT NULL,"
            " mono_ms INTEGER NOT NULL,utc_ms INTEGER NOT NULL,detail_json TEXT NOT NULL,created_at INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS display_capture_evidence(evidence_id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,"
            " idx INTEGER NOT NULL,target_offset_ms INTEGER NOT NULL,actual_offset_ms INTEGER NOT NULL,status TEXT NOT NULL,"
            " failure_reason TEXT NOT NULL DEFAULT '',source TEXT NOT NULL,path TEXT NOT NULL DEFAULT '',phash INTEGER NOT NULL DEFAULT 0,"
            " sha256 TEXT NOT NULL DEFAULT '',width INTEGER NOT NULL DEFAULT 0,height INTEGER NOT NULL DEFAULT 0,bytes INTEGER NOT NULL DEFAULT 0,"
            " took_ms INTEGER NOT NULL DEFAULT 0,utc_ms INTEGER NOT NULL,model_id INTEGER NOT NULL DEFAULT 0,embedding BLOB,"
            " embedded INTEGER NOT NULL DEFAULT 0,image_available INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,"
            " capture_method TEXT NOT NULL DEFAULT 'brightscript_screenshot',provenance TEXT NOT NULL DEFAULT '',uncertainty_ms INTEGER NOT NULL DEFAULT -1,"
            " UNIQUE(session_id,idx));"
            "CREATE TABLE IF NOT EXISTS playback_verifications(verification_id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL UNIQUE,"
            " status TEXT NOT NULL,reason TEXT NOT NULL,expected_media_id TEXT NOT NULL,best_media_id TEXT NOT NULL,frames_total INTEGER NOT NULL,"
            " frames_ok INTEGER NOT NULL,frames_matched INTEGER NOT NULL,frames_mismatched INTEGER NOT NULL,mean_sim_expected REAL NOT NULL,"
            " min_sim_expected REAL NOT NULL,mean_phash_dist REAL NOT NULL,playback_reported INTEGER NOT NULL,playback_complete INTEGER NOT NULL,"
            " actual_play_ms INTEGER NOT NULL,slot_ms INTEGER NOT NULL,details_json TEXT NOT NULL,engine_version TEXT NOT NULL,created_at INTEGER NOT NULL);"
            "CREATE INDEX IF NOT EXISTS ix_pop_ev_session ON playback_events(session_id,event_id);"
            "CREATE INDEX IF NOT EXISTS ix_pop_evd_session ON display_capture_evidence(session_id,idx);"
            "CREATE INDEX IF NOT EXISTS ix_pop_evd_time ON display_capture_evidence(utc_ms);"
            "CREATE INDEX IF NOT EXISTS ix_pop_sess_time ON playback_sessions(created_at);"
            "CREATE INDEX IF NOT EXISTS ix_pop_sess_media ON playback_sessions(expected_media_id);"
            "PRAGMA user_version=2;";
        if (!exec(ddl, err)) { std::string e2; exec("ROLLBACK;", e2); return false; }
        return exec("COMMIT;", err);
    }
    int64_t model_id(const EmbeddingSpec& s, bool create) {
        {
            Stmt q(db, "SELECT model_id FROM pop_models WHERE model_name=? AND model_version=? AND model_sha256=? AND preprocessing=? AND dimension=? AND dtype=? AND metric=?;");
            if (!q) return 0;
            q.text(1, s.model_name); q.text(2, s.model_version); q.text(3, s.model_sha256); q.text(4, s.preprocessing);
            sqlite3_bind_int(q.s, 5, s.dimension); q.text(6, s.dtype); q.text(7, s.metric);
            if (sqlite3_step(q.s) == SQLITE_ROW) return sqlite3_column_int64(q.s, 0);
        }
        if (!create) return 0;
        Stmt ins(db, "INSERT INTO pop_models(model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric) VALUES(?,?,?,?,?,?,?);");
        if (!ins) return 0;
        ins.text(1, s.model_name); ins.text(2, s.model_version); ins.text(3, s.model_sha256); ins.text(4, s.preprocessing);
        sqlite3_bind_int(ins.s, 5, s.dimension); ins.text(6, s.dtype); ins.text(7, s.metric);
        return sqlite3_step(ins.s) == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : 0;
    }
    bool session_exists(const std::string& id) {
        Stmt q(db, "SELECT 1 FROM playback_sessions WHERE session_id=?;");
        if (!q) return false;
        q.text(1, id);
        return sqlite3_step(q.s) == SQLITE_ROW;
    }
    uint64_t count(const char* sql) {
        Stmt q(db, sql);
        return (q && sqlite3_step(q.s) == SQLITE_ROW) ? static_cast<uint64_t>(sqlite3_column_int64(q.s, 0)) : 0;
    }
    bool load_session(const std::string& id, PopSession& out) {
        Stmt q(db, std::string("SELECT ") + kSessionCols + " FROM playback_sessions WHERE session_id=?;");
        if (!q) return false;
        q.text(1, id);
        if (sqlite3_step(q.s) != SQLITE_ROW) return false;
        out = row_session(q.s);
        return true;
    }
};

PopStore::PopStore(PopConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
PopStore::~PopStore() { close(); }
std::string PopStore::root() const { return impl_->cfg.root_directory; }

bool PopStore::open(std::string& err) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->db) return true;
    std::error_code ec;
    fs::create_directories(impl_->cfg.root_directory, ec);
    const std::string path = (fs::path(impl_->cfg.root_directory) / "pop.db").string();
    if (sqlite3_open_v2(path.c_str(), &impl_->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        err = impl_->db ? sqlite3_errmsg(impl_->db) : "sqlite3_open failed";
        if (impl_->db) { sqlite3_close(impl_->db); impl_->db = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(impl_->db, impl_->cfg.busy_timeout_ms);
    std::string e;
    impl_->exec(impl_->cfg.sync_full ? "PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;" : "PRAGMA foreign_keys=ON; PRAGMA synchronous=NORMAL;", e);
    if (!impl_->migrate(err)) { sqlite3_close(impl_->db); impl_->db = nullptr; return false; }
    {   // history recorded earlier stays searchable even while recording is switched off; nothing is created here
        Stmt q(impl_->db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='display_history_frames';");
        impl_->hist = impl_->v2 && q && sqlite3_step(q.s) == SQLITE_ROW;
    }
    return true;
}
bool PopStore::schema_v2() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->v2; }
bool PopStore::is_open() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->db != nullptr; }
void PopStore::close() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->db) { sqlite3_close(impl_->db); impl_->db = nullptr; }
}

PopSummary PopStore::summary() {
    PopSummary s;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return s;
    s.open = true; s.schema_version = impl_->user_version();
    s.media = impl_->count("SELECT COUNT(*) FROM media_assets;");
    s.reference_frames = impl_->count("SELECT COUNT(*) FROM media_reference_frames;");
    s.sessions = impl_->count("SELECT COUNT(*) FROM playback_sessions;");
    s.events = impl_->count("SELECT COUNT(*) FROM playback_events;");
    s.evidence = impl_->count("SELECT COUNT(*) FROM display_capture_evidence;");
    s.evidence_bytes = impl_->count("SELECT COALESCE(SUM(bytes),0) FROM display_capture_evidence WHERE image_available=1;");
    s.verifications = impl_->count("SELECT COUNT(*) FROM playback_verifications;");
    Stmt q(impl_->db, "SELECT COALESCE(MIN(created_at),0),COALESCE(MAX(created_at),0) FROM playback_sessions;");
    if (q && sqlite3_step(q.s) == SQLITE_ROW) { s.oldest_session_utc_ms = sqlite3_column_int64(q.s, 0); s.newest_session_utc_ms = sqlite3_column_int64(q.s, 1); }
    std::error_code ec;
    const auto sz = fs::file_size(fs::path(impl_->cfg.root_directory) / "pop.db", ec);
    if (!ec) s.db_bytes = sz;
    return s;
}

// ---- reference library ------------------------------------------------------------------------------------------
bool PopStore::valid_media_id(const std::string& id) const { return valid_id(id); }

bool PopStore::register_media(const MediaAsset& a, std::string& err) {
    if (!valid_id(a.media_id)) { err = "media_id must be 1-64 characters of [A-Za-z0-9._-]"; return false; }
    if (a.kind != "file" && a.kind != "rtsp") { err = "kind must be 'file' or 'rtsp'"; return false; }
    if (a.uri.empty() || a.uri.size() > 512) { err = "uri is required (max 512 characters)"; return false; }
    if (!(a.slot_s > 0 && a.slot_s <= 3600)) { err = "slot_s must be in (0, 3600]"; return false; }
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) { err = "store not open"; return false; }
    Stmt q(impl_->db,
           "INSERT INTO media_assets(media_id,display_name,kind,uri,sha256,codec,duration_s,slot_s,width,height,fps,created_at,updated_at) "
           "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(media_id) DO UPDATE SET display_name=excluded.display_name,kind=excluded.kind,"
           "uri=excluded.uri,sha256=excluded.sha256,codec=excluded.codec,duration_s=excluded.duration_s,slot_s=excluded.slot_s,width=excluded.width,"
           "height=excluded.height,fps=excluded.fps,updated_at=excluded.updated_at;");
    if (!q) { err = sqlite3_errmsg(impl_->db); return false; }
    const int64_t t = now_ms();
    q.text(1, a.media_id); q.text(2, a.display_name.empty() ? a.media_id : a.display_name); q.text(3, a.kind); q.text(4, a.uri);
    q.text(5, a.sha256); q.text(6, a.codec); sqlite3_bind_double(q.s, 7, a.duration_s); sqlite3_bind_double(q.s, 8, a.slot_s);
    sqlite3_bind_int(q.s, 9, a.width); sqlite3_bind_int(q.s, 10, a.height); sqlite3_bind_double(q.s, 11, a.fps);
    sqlite3_bind_int64(q.s, 12, t); sqlite3_bind_int64(q.s, 13, t);
    if (sqlite3_step(q.s) != SQLITE_DONE) { err = sqlite3_errmsg(impl_->db); return false; }
    return true;
}

static MediaAsset row_media(sqlite3_stmt* s) {
    MediaAsset m;
    m.media_id = col_text(s, 0); m.display_name = col_text(s, 1); m.kind = col_text(s, 2); m.uri = col_text(s, 3); m.sha256 = col_text(s, 4);
    m.codec = col_text(s, 5); m.duration_s = sqlite3_column_double(s, 6); m.slot_s = sqlite3_column_double(s, 7); m.width = sqlite3_column_int(s, 8);
    m.height = sqlite3_column_int(s, 9); m.fps = sqlite3_column_double(s, 10); m.created_utc_ms = sqlite3_column_int64(s, 11);
    m.updated_utc_ms = sqlite3_column_int64(s, 12); m.reference_frames = static_cast<uint64_t>(sqlite3_column_int64(s, 13));
    return m;
}
static const char* kMediaSel =
    "SELECT a.media_id,a.display_name,a.kind,a.uri,a.sha256,a.codec,a.duration_s,a.slot_s,a.width,a.height,a.fps,a.created_at,a.updated_at,"
    "(SELECT COUNT(*) FROM media_reference_frames r WHERE r.media_id=a.media_id) FROM media_assets a";

bool PopStore::get_media(const std::string& id, MediaAsset& out) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    Stmt q(impl_->db, std::string(kMediaSel) + " WHERE a.media_id=?;");
    if (!q) return false;
    q.text(1, id);
    if (sqlite3_step(q.s) != SQLITE_ROW) return false;
    out = row_media(q.s);
    return true;
}
std::vector<MediaAsset> PopStore::list_media() {
    std::vector<MediaAsset> v;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return v;
    Stmt q(impl_->db, std::string(kMediaSel) + " ORDER BY a.media_id;");
    const std::string suffix = kDisplayProfileSuffix();
    while (q && sqlite3_step(q.s) == SQLITE_ROW) {
        MediaAsset m = row_media(q.s);
        if (m.media_id.size() > suffix.size() && m.media_id.compare(m.media_id.size() - suffix.size(), suffix.size(), suffix) == 0) continue;   // hidden profile row
        v.push_back(std::move(m));
    }
    return v;
}
bool PopStore::delete_media(const std::string& id) {
    if (!valid_id(id)) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    Stmt q(impl_->db, "DELETE FROM media_assets WHERE media_id=?;");
    if (!q) return false;
    q.text(1, id);
    const bool ok = sqlite3_step(q.s) == SQLITE_DONE && sqlite3_changes(impl_->db) > 0;
    if (ok) { std::error_code ec; fs::remove_all(fs::path(impl_->cfg.root_directory) / "pop" / "refs" / id, ec); }
    return ok;
}

bool PopStore::add_reference(EmbeddingProvider& provider, const std::string& media_id, int offset_ms,
                             const std::vector<uint8_t>& jpeg, std::string& err) {
    if (!valid_id(media_id)) { err = "invalid media_id"; return false; }
    if (offset_ms < 0 || offset_ms > 24 * 3600 * 1000) { err = "offset_ms out of range"; return false; }
    if (jpeg.empty() || jpeg.size() > kMaxJpegBytes) { err = "reference image missing or larger than 8 MiB"; return false; }
    MediaAsset m;
    if (!get_media(media_id, m)) { err = "unknown media_id (register the media first)"; return false; }
    std::vector<float> emb; int w = 0, h = 0;
    if (!decode_embed(provider, jpeg, emb, w, h, err)) return false;
    uint64_t ph = 0;
    if (!phash_of_jpeg(jpeg, ph, err)) return false;
    const std::string rel = "pop/refs/" + media_id + "/" + std::to_string(offset_ms) + ".jpg";
    if (!write_atomic(fs::path(impl_->cfg.root_directory) / rel, jpeg, err)) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) { err = "store not open"; return false; }
    const int64_t mid = impl_->model_id(provider.spec(), true);
    if (mid == 0) { err = "cannot register model"; return false; }
    Stmt q(impl_->db, "INSERT INTO media_reference_frames(media_id,offset_ms,phash,path,width,height,model_id,embedding,created_at) VALUES(?,?,?,?,?,?,?,?,?) "
                      "ON CONFLICT(media_id,offset_ms,model_id) DO UPDATE SET phash=excluded.phash,path=excluded.path,width=excluded.width,"
                      "height=excluded.height,embedding=excluded.embedding,created_at=excluded.created_at;");
    if (!q) { err = sqlite3_errmsg(impl_->db); return false; }
    q.text(1, media_id); sqlite3_bind_int(q.s, 2, offset_ms); sqlite3_bind_int64(q.s, 3, as_i64(ph)); q.text(4, rel);
    sqlite3_bind_int(q.s, 5, w); sqlite3_bind_int(q.s, 6, h); sqlite3_bind_int64(q.s, 7, mid); bind_vec(q.s, 8, emb); sqlite3_bind_int64(q.s, 9, now_ms());
    if (sqlite3_step(q.s) != SQLITE_DONE) { err = sqlite3_errmsg(impl_->db); return false; }
    return true;
}

std::vector<ReferenceFrame> PopStore::list_references(const std::string& media_id) {
    std::vector<ReferenceFrame> v;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return v;
    Stmt q(impl_->db, "SELECT ref_id,media_id,offset_ms,phash,path,width,height FROM media_reference_frames WHERE media_id=? ORDER BY offset_ms;");
    if (!q) return v;
    q.text(1, media_id);
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        ReferenceFrame r; r.ref_id = sqlite3_column_int64(q.s, 0); r.media_id = col_text(q.s, 1); r.offset_ms = sqlite3_column_int(q.s, 2);
        r.phash = as_u64(sqlite3_column_int64(q.s, 3)); r.path = col_text(q.s, 4); r.width = sqlite3_column_int(q.s, 5); r.height = sqlite3_column_int(q.s, 6);
        v.push_back(std::move(r));
    }
    return v;
}
bool PopStore::clear_references(const std::string& media_id) {
    if (!valid_id(media_id)) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    Stmt q(impl_->db, "DELETE FROM media_reference_frames WHERE media_id=?;");
    if (!q) return false;
    q.text(1, media_id);
    const bool ok = sqlite3_step(q.s) == SQLITE_DONE;
    if (ok) { std::error_code ec; fs::remove_all(fs::path(impl_->cfg.root_directory) / "pop" / "refs" / media_id, ec); }
    return ok;
}
bool PopStore::reference_jpeg(int64_t ref_id, std::vector<uint8_t>& out) {
    std::string rel;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->db) return false;
        Stmt q(impl_->db, "SELECT path FROM media_reference_frames WHERE ref_id=?;");
        if (!q) return false;
        sqlite3_bind_int64(q.s, 1, ref_id);
        if (sqlite3_step(q.s) != SQLITE_ROW) return false;
        rel = col_text(q.s, 0);
    }
    std::string err;
    return detail::read_file_bounded((fs::path(impl_->cfg.root_directory) / rel).string(), kMaxJpegBytes, out, err);
}

// ---- sessions and events ----------------------------------------------------------------------------------------
bool PopStore::schedule_session(const PopSession& s, std::string& err) {
    if (!valid_id(s.session_id)) { err = "session_id must be 1-64 characters of [A-Za-z0-9._-]"; return false; }
    if (!valid_id(s.expected_media_id)) { err = "expected_media_id is required"; return false; }
    if (s.slot_ms < 1000 || s.slot_ms > 3600000) { err = "slot_ms must be between 1000 and 3600000"; return false; }
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) { err = "store not open"; return false; }
    if (impl_->session_exists(s.session_id)) { err = "session already exists"; return false; }
    Stmt q(impl_->db, "INSERT INTO playback_sessions(session_id,job_id,expected_media_id,source,state,scheduled_utc_ms,slot_ms,captures_requested,created_at) "
                      "VALUES(?,?,?,?, 'scheduled',?,?,?,?);");
    if (!q) { err = sqlite3_errmsg(impl_->db); return false; }
    q.text(1, s.session_id); q.text(2, s.job_id); q.text(3, s.expected_media_id); q.text(4, s.source.empty() ? "main_display" : s.source);
    sqlite3_bind_int64(q.s, 5, s.scheduled_utc_ms ? s.scheduled_utc_ms : now_ms()); sqlite3_bind_int64(q.s, 6, s.slot_ms);
    sqlite3_bind_int(q.s, 7, s.captures_requested); sqlite3_bind_int64(q.s, 8, now_ms());
    if (sqlite3_step(q.s) != SQLITE_DONE) { err = sqlite3_errmsg(impl_->db); return false; }
    return true;
}
bool PopStore::get_session(const std::string& id, PopSession& out) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->db && impl_->load_session(id, out);
}
std::vector<PopSession> PopStore::list_sessions(const SessionFilter& f, int limit, int offset, uint64_t* total) {
    std::vector<PopSession> v;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return v;
    std::string where = " WHERE 1=1";
    if (!f.expected_media_id.empty()) where += " AND s.expected_media_id=?";
    if (!f.status.empty()) where += " AND COALESCE((SELECT status FROM playback_verifications p WHERE p.session_id=s.session_id),'scheduled')=?";
    if (f.start_utc_ms > 0) where += " AND s.created_at>=?";
    if (f.end_utc_ms > 0) where += " AND s.created_at<?";
    auto bind = [&](Stmt& q) {
        int i = 1;
        if (!f.expected_media_id.empty()) q.text(i++, f.expected_media_id);
        if (!f.status.empty()) q.text(i++, f.status);
        if (f.start_utc_ms > 0) sqlite3_bind_int64(q.s, i++, f.start_utc_ms);
        if (f.end_utc_ms > 0) sqlite3_bind_int64(q.s, i++, f.end_utc_ms);
        return i;
    };
    if (total) {
        Stmt c(impl_->db, "SELECT COUNT(*) FROM playback_sessions s" + where + ";");
        if (c) { bind(c); *total = sqlite3_step(c.s) == SQLITE_ROW ? static_cast<uint64_t>(sqlite3_column_int64(c.s, 0)) : 0; }
    }
    std::string cols;
    for (const char* p = kSessionCols; *p; ++p) { if (p == kSessionCols || *(p - 1) == ',') cols += "s."; cols += *p; }
    Stmt q(impl_->db, "SELECT " + cols + " FROM playback_sessions s" + where + " ORDER BY s.created_at DESC,s.session_id DESC LIMIT ? OFFSET ?;");
    if (!q) return v;
    const int i = bind(q);
    sqlite3_bind_int(q.s, i, std::clamp(limit, 1, 500)); sqlite3_bind_int(q.s, i + 1, std::max(offset, 0));
    while (sqlite3_step(q.s) == SQLITE_ROW) v.push_back(row_session(q.s));
    return v;
}

bool PopStore::apply_event(const PopEventIn& e, std::string& err) {
    if (!valid_id(e.session_id)) { err = "invalid session_id"; return false; }
    if (e.type.empty() || e.type.size() > 40) { err = "invalid event type"; return false; }
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) { err = "store not open"; return false; }
    if (!impl_->exec("BEGIN IMMEDIATE;", err)) return false;
    auto rollback = [&] { std::string x; impl_->exec("ROLLBACK;", x); };
    if (!impl_->session_exists(e.session_id)) {                      // ad-hoc session (no schedule): expected = reported media
        const std::string exp = !e.expected_media_id.empty() ? e.expected_media_id : (e.played_media_id.empty() ? "unknown" : e.played_media_id);
        Stmt ins(impl_->db, "INSERT INTO playback_sessions(session_id,job_id,expected_media_id,state,scheduled_utc_ms,created_at) VALUES(?,?,?,'scheduled',?,?);");
        if (!ins) { err = sqlite3_errmsg(impl_->db); rollback(); return false; }
        ins.text(1, e.session_id); ins.text(2, e.job_id); ins.text(3, exp); sqlite3_bind_int64(ins.s, 4, e.utc_ms); sqlite3_bind_int64(ins.s, 5, now_ms());
        if (sqlite3_step(ins.s) != SQLITE_DONE) { err = sqlite3_errmsg(impl_->db); rollback(); return false; }
    }
    {
        Stmt q(impl_->db, "INSERT INTO playback_events(session_id,type,mono_ms,utc_ms,detail_json,created_at) VALUES(?,?,?,?,?,?);");
        if (!q) { err = sqlite3_errmsg(impl_->db); rollback(); return false; }
        q.text(1, e.session_id); q.text(2, e.type); sqlite3_bind_int64(q.s, 3, e.mono_ms); sqlite3_bind_int64(q.s, 4, e.utc_ms);
        q.text(5, e.detail_json.size() > 4096 ? std::string("{}") : e.detail_json); sqlite3_bind_int64(q.s, 6, now_ms());
        if (sqlite3_step(q.s) != SQLITE_DONE) { err = sqlite3_errmsg(impl_->db); rollback(); return false; }
    }
    bool ok = true;
    if (e.type == "session_scheduled_run") {
        Stmt q(impl_->db, "UPDATE playback_sessions SET state='running',job_id=CASE WHEN ?1<>'' THEN ?1 ELSE job_id END,"
                          "played_media_id=CASE WHEN ?2<>'' THEN ?2 ELSE played_media_id END,"
                          "captures_requested=CASE WHEN ?3>=0 THEN ?3 ELSE captures_requested END WHERE session_id=?4 AND state<>'ended';");
        ok = false;
        if (q) {
            q.text(1, e.job_id); q.text(2, e.played_media_id); sqlite3_bind_int(q.s, 3, e.captures_requested); q.text(4, e.session_id);
            ok = sqlite3_step(q.s) == SQLITE_DONE;
        }
    } else if (e.type == "play_started") {
        Stmt q(impl_->db, "UPDATE playback_sessions SET state='running',start_utc_ms=CASE WHEN start_utc_ms=0 THEN ?1 ELSE start_utc_ms END WHERE session_id=?2 AND state<>'ended';");
        ok = false;
        if (q) { sqlite3_bind_int64(q.s, 1, e.utc_ms); q.text(2, e.session_id); ok = sqlite3_step(q.s) == SQLITE_DONE; }
    } else if (e.type == "session_end") {
        Stmt q(impl_->db, "UPDATE playback_sessions SET state='ended',end_utc_ms=?1,actual_play_ms=?2,end_reason=?3 WHERE session_id=?4;");
        ok = false;
        if (q) { sqlite3_bind_int64(q.s, 1, e.utc_ms); sqlite3_bind_int64(q.s, 2, e.actual_play_ms); q.text(3, e.reason); q.text(4, e.session_id); ok = sqlite3_step(q.s) == SQLITE_DONE; }
    }
    if (!ok) { err = sqlite3_errmsg(impl_->db); rollback(); return false; }
    return impl_->exec("COMMIT;", err);
}
std::vector<PopEvent> PopStore::list_events(const std::string& session_id) {
    std::vector<PopEvent> v;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return v;
    Stmt q(impl_->db, "SELECT event_id,session_id,type,mono_ms,utc_ms,detail_json FROM playback_events WHERE session_id=? ORDER BY event_id;");
    if (!q) return v;
    q.text(1, session_id);
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        PopEvent e; e.event_id = sqlite3_column_int64(q.s, 0); e.session_id = col_text(q.s, 1); e.type = col_text(q.s, 2);
        e.mono_ms = sqlite3_column_int64(q.s, 3); e.utc_ms = sqlite3_column_int64(q.s, 4); e.detail_json = col_text(q.s, 5);
        v.push_back(std::move(e));
    }
    return v;
}

// ---- evidence ---------------------------------------------------------------------------------------------------
std::string PopStore::partition_path(const std::string& source, int64_t utc_ms, const std::string& file_name) {
    std::time_t t = static_cast<std::time_t>(utc_ms / 1000);
    std::tm tmv{};
    gmtime_r(&t, &tmv);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "pop/evidence/%04d/%s/%s/%s/%s/%s", tmv.tm_year + 1900, two(tmv.tm_mon + 1).c_str(), two(tmv.tm_mday).c_str(),
                  two(tmv.tm_hour).c_str(), safe_segment(source).c_str(), safe_segment(file_name).c_str());
    return buf;
}

bool PopStore::add_evidence(EmbeddingProvider& provider, const EvidenceIn& in, Evidence& out, std::string& err) {
    if (!valid_id(in.session_id)) { err = "invalid session_id"; return false; }
    if (in.idx < 1 || in.idx > 1000) { err = "idx must be 1..1000"; return false; }
    if (in.status != "ok" && in.status != "failed") { err = "status must be 'ok' or 'failed'"; return false; }
    out = Evidence{};
    out.session_id = in.session_id; out.idx = in.idx; out.target_offset_ms = in.target_offset_ms; out.actual_offset_ms = in.actual_offset_ms;
    out.status = in.status; out.failure_reason = in.failure_reason.substr(0, 200); out.source = in.source.empty() ? "main_display" : in.source;
    out.took_ms = in.took_ms; out.utc_ms = in.utc_ms ? in.utc_ms : now_ms();
    out.capture_method = in.capture_method.empty() ? "brightscript_screenshot" : in.capture_method.substr(0, 40);
    out.provenance = in.provenance.substr(0, 120); out.uncertainty_ms = in.uncertainty_ms;
    std::vector<float> emb; int64_t mid = 0;
    if (in.status == "ok") {
        if (in.jpeg.empty() || in.jpeg.size() > kMaxJpegBytes) { err = "screenshot missing or larger than 8 MiB"; return false; }
        int w = 0, h = 0;
        if (!decode_embed(provider, in.jpeg, emb, w, h, err)) return false;
        std::string e2;
        if (!phash_of_jpeg(in.jpeg, out.phash, e2)) { err = e2; return false; }
        out.width = w; out.height = h; out.bytes = in.jpeg.size(); out.embedded = true;
        out.sha256 = detail::sha256_hex(in.jpeg.data(), in.jpeg.size());
        out.path = PopStore::partition_path(out.source, out.utc_ms, in.session_id + "_" + std::to_string(in.idx) + ".jpg");
        if (!write_atomic(fs::path(impl_->cfg.root_directory) / out.path, in.jpeg, err)) return false;
        out.image_available = true;
    }
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) { err = "store not open"; return false; }
    if (out.embedded) { mid = impl_->model_id(provider.spec(), true); if (mid == 0) { err = "cannot register model"; return false; } }
    if (!impl_->v2 && out.capture_method != "brightscript_screenshot") { err = "pop.db is schema 1: evidence from " + out.capture_method + " needs visual_memory.pop.allow_schema_upgrade"; return false; }
    const std::string v2cols = impl_->v2 ? ",capture_method,provenance,uncertainty_ms" : "";
    const std::string v2vals = impl_->v2 ? ",?,?,?" : "";
    const std::string v2upd = impl_->v2 ? ",capture_method=excluded.capture_method,provenance=excluded.provenance,uncertainty_ms=excluded.uncertainty_ms" : "";
    Stmt q(impl_->db, std::string("INSERT INTO display_capture_evidence(session_id,idx,target_offset_ms,actual_offset_ms,status,failure_reason,source,path,phash,sha256,"
                      "width,height,bytes,took_ms,utc_ms,model_id,embedding,embedded,image_available,created_at") + v2cols + ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?" + v2vals + ") "
                      "ON CONFLICT(session_id,idx) DO UPDATE SET target_offset_ms=excluded.target_offset_ms,actual_offset_ms=excluded.actual_offset_ms,"
                      "status=excluded.status,failure_reason=excluded.failure_reason,source=excluded.source,path=excluded.path,phash=excluded.phash,"
                      "sha256=excluded.sha256,width=excluded.width,height=excluded.height,bytes=excluded.bytes,took_ms=excluded.took_ms,utc_ms=excluded.utc_ms,"
                      "model_id=excluded.model_id,embedding=excluded.embedding,embedded=excluded.embedded,image_available=excluded.image_available" + v2upd + ";");
    if (!q) { err = sqlite3_errmsg(impl_->db); return false; }
    q.text(1, out.session_id); sqlite3_bind_int(q.s, 2, out.idx); sqlite3_bind_int(q.s, 3, out.target_offset_ms); sqlite3_bind_int(q.s, 4, out.actual_offset_ms);
    q.text(5, out.status); q.text(6, out.failure_reason); q.text(7, out.source); q.text(8, out.path); sqlite3_bind_int64(q.s, 9, as_i64(out.phash));
    q.text(10, out.sha256); sqlite3_bind_int(q.s, 11, out.width); sqlite3_bind_int(q.s, 12, out.height); sqlite3_bind_int64(q.s, 13, static_cast<int64_t>(out.bytes));
    sqlite3_bind_int(q.s, 14, out.took_ms); sqlite3_bind_int64(q.s, 15, out.utc_ms); sqlite3_bind_int64(q.s, 16, mid);
    if (out.embedded) bind_vec(q.s, 17, emb); else sqlite3_bind_null(q.s, 17);
    sqlite3_bind_int(q.s, 18, out.embedded ? 1 : 0); sqlite3_bind_int(q.s, 19, out.image_available ? 1 : 0); sqlite3_bind_int64(q.s, 20, now_ms());
    if (impl_->v2) { q.text(21, out.capture_method); q.text(22, out.provenance); sqlite3_bind_int(q.s, 23, out.uncertainty_ms); }
    if (sqlite3_step(q.s) != SQLITE_DONE) { err = sqlite3_errmsg(impl_->db); return false; }
    Stmt id(impl_->db, "SELECT evidence_id FROM display_capture_evidence WHERE session_id=? AND idx=?;");
    if (id) { id.text(1, out.session_id); sqlite3_bind_int(id.s, 2, out.idx); if (sqlite3_step(id.s) == SQLITE_ROW) out.evidence_id = sqlite3_column_int64(id.s, 0); }
    return true;
}
std::vector<Evidence> PopStore::list_evidence(const std::string& session_id) {
    std::vector<Evidence> v;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return v;
    Stmt q(impl_->db, std::string("SELECT ") + (impl_->v2 ? kEvidenceCols : kEvidenceColsV1) + " FROM display_capture_evidence WHERE session_id=? ORDER BY idx;");
    if (!q) return v;
    q.text(1, session_id);
    while (sqlite3_step(q.s) == SQLITE_ROW) v.push_back(row_evidence(q.s));
    return v;
}
bool PopStore::evidence_jpeg(int64_t evidence_id, std::vector<uint8_t>& out) {
    std::string rel;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->db) return false;
        Stmt q(impl_->db, "SELECT path FROM display_capture_evidence WHERE evidence_id=? AND image_available=1;");
        if (!q) return false;
        sqlite3_bind_int64(q.s, 1, evidence_id);
        if (sqlite3_step(q.s) != SQLITE_ROW) return false;
        rel = col_text(q.s, 0);
    }
    std::string err;
    return !rel.empty() && detail::read_file_bounded((fs::path(impl_->cfg.root_directory) / rel).string(), kMaxJpegBytes, out, err);
}

// ---- verification -----------------------------------------------------------------------------------------------
bool PopStore::verify_session(const std::string& session_id, const VerifyThresholds& t, Verification& out, std::string& err) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) { err = "store not open"; return false; }
    PopSession s;
    if (!impl_->load_session(session_id, s)) { err = "unknown session"; return false; }
    VerifyInput in;
    in.expected_media_id = s.expected_media_id; in.slot_ms = s.slot_ms; in.actual_play_ms = s.actual_play_ms;
    in.ended = s.state == "ended"; in.end_reason = s.end_reason; in.captures_requested = s.captures_requested;
    {
        Stmt q(impl_->db, "SELECT type FROM playback_events WHERE session_id=?;");
        if (q) {
            q.text(1, session_id);
            while (sqlite3_step(q.s) == SQLITE_ROW) {
                const std::string ty = col_text(q.s, 0);
                if (ty == "play_requested" || ty == "play_started" || ty == "play_ended" || ty == "play_stopped" || ty == "play_error" || ty == "session_end") in.reported = true;
            }
        }
    }
    int64_t model = 0;
    bool display_evidence = false;                      // any evidence captured from the OS rendered frames
    {
        Stmt q(impl_->db, impl_->v2 ? "SELECT COUNT(*) FROM display_capture_evidence WHERE session_id=? AND capture_method='resilience_grpc' AND status='ok';" : "SELECT 0 WHERE ?<>'';");
        if (q) { q.text(1, session_id); if (sqlite3_step(q.s) == SQLITE_ROW) display_evidence = sqlite3_column_int(q.s, 0) > 0; }
    }
    {
        Stmt q(impl_->db, "SELECT evidence_id,idx,actual_offset_ms,status,phash,model_id,embedding FROM display_capture_evidence WHERE session_id=? ORDER BY idx;");
        if (!q) { err = sqlite3_errmsg(impl_->db); return false; }
        q.text(1, session_id);
        while (sqlite3_step(q.s) == SQLITE_ROW) {
            if (col_text(q.s, 3) != "ok" || sqlite3_column_type(q.s, 6) == SQLITE_NULL) { ++in.captures_failed; continue; }
            VerifyInput::Ev e; e.evidence_id = sqlite3_column_int64(q.s, 0); e.idx = sqlite3_column_int(q.s, 1); e.actual_offset_ms = sqlite3_column_int(q.s, 2);
            e.phash = as_u64(sqlite3_column_int64(q.s, 4)); e.emb = blob_vec(q.s, 6); model = sqlite3_column_int64(q.s, 5);
            in.evidence.push_back(std::move(e));
        }
    }
    if (model != 0) {
        Stmt q(impl_->db, "SELECT media_id,offset_ms,phash,embedding FROM media_reference_frames WHERE model_id=?;");
        if (q) {
            sqlite3_bind_int64(q.s, 1, model);
            while (sqlite3_step(q.s) == SQLITE_ROW) {
                VerifyInput::Ref r; r.media_id = col_text(q.s, 0); r.offset_ms = sqlite3_column_int(q.s, 1); r.phash = as_u64(sqlite3_column_int64(q.s, 2)); r.emb = blob_vec(q.s, 3);
                in.refs.push_back(std::move(r));
            }
        }
        // Reference geometry: OS rendered frames are compared with the references built for that geometry (media id + suffix) when they
        // exist; everything else uses the screenshot references. Never mix the two sets for one verification.
        const std::string suffix = kDisplayProfileSuffix();
        auto is_display = [&](const std::string& id) { return id.size() > suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0; };
        std::set<std::string> have_display;
        for (const auto& r : in.refs) if (is_display(r.media_id)) have_display.insert(r.media_id.substr(0, r.media_id.size() - suffix.size()));
        std::vector<VerifyInput::Ref> chosen;
        for (auto& r : in.refs) {
            if (is_display(r.media_id)) {
                if (display_evidence) { r.media_id = r.media_id.substr(0, r.media_id.size() - suffix.size()); chosen.push_back(std::move(r)); }
            } else if (!display_evidence || !have_display.count(r.media_id)) chosen.push_back(std::move(r));   // fall back to screenshot references when no display set exists
        }
        in.refs = std::move(chosen);
    }
    out = verify_playback(in, t);
    out.session_id = session_id;
    const std::string json = verification_to_json(out);
    if (!impl_->exec("BEGIN IMMEDIATE;", err)) return false;
    Stmt q(impl_->db, "INSERT INTO playback_verifications(session_id,status,reason,expected_media_id,best_media_id,frames_total,frames_ok,frames_matched,frames_mismatched,"
                      "mean_sim_expected,min_sim_expected,mean_phash_dist,playback_reported,playback_complete,actual_play_ms,slot_ms,details_json,engine_version,created_at) "
                      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(session_id) DO UPDATE SET status=excluded.status,reason=excluded.reason,"
                      "expected_media_id=excluded.expected_media_id,best_media_id=excluded.best_media_id,frames_total=excluded.frames_total,frames_ok=excluded.frames_ok,"
                      "frames_matched=excluded.frames_matched,frames_mismatched=excluded.frames_mismatched,mean_sim_expected=excluded.mean_sim_expected,"
                      "min_sim_expected=excluded.min_sim_expected,mean_phash_dist=excluded.mean_phash_dist,playback_reported=excluded.playback_reported,"
                      "playback_complete=excluded.playback_complete,actual_play_ms=excluded.actual_play_ms,slot_ms=excluded.slot_ms,details_json=excluded.details_json,"
                      "engine_version=excluded.engine_version,created_at=excluded.created_at;");
    bool ok = static_cast<bool>(q);
    if (ok) {
        q.text(1, session_id); q.text(2, to_string(out.status)); q.text(3, out.reason); q.text(4, out.expected_media_id); q.text(5, out.best_media_id);
        sqlite3_bind_int(q.s, 6, out.frames_total); sqlite3_bind_int(q.s, 7, out.frames_ok); sqlite3_bind_int(q.s, 8, out.frames_matched); sqlite3_bind_int(q.s, 9, out.frames_mismatched);
        sqlite3_bind_double(q.s, 10, out.mean_sim_expected); sqlite3_bind_double(q.s, 11, out.min_sim_expected); sqlite3_bind_double(q.s, 12, out.mean_phash_dist);
        sqlite3_bind_int(q.s, 13, out.playback_reported); sqlite3_bind_int(q.s, 14, out.playback_complete); sqlite3_bind_int64(q.s, 15, out.actual_play_ms);
        sqlite3_bind_int64(q.s, 16, out.slot_ms); q.text(17, json); q.text(18, out.engine_version); sqlite3_bind_int64(q.s, 19, out.created_utc_ms);
        ok = sqlite3_step(q.s) == SQLITE_DONE;
    }
    if (!ok) { err = sqlite3_errmsg(impl_->db); std::string x; impl_->exec("ROLLBACK;", x); return false; }
    return impl_->exec("COMMIT;", err);
}

bool PopStore::get_verification(const std::string& session_id, Verification& out) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    Stmt q(impl_->db, "SELECT status,reason,expected_media_id,best_media_id,frames_total,frames_ok,frames_matched,frames_mismatched,mean_sim_expected,"
                      "min_sim_expected,mean_phash_dist,playback_reported,playback_complete,actual_play_ms,slot_ms,engine_version,created_at,verification_id "
                      "FROM playback_verifications WHERE session_id=?;");
    if (!q) return false;
    q.text(1, session_id);
    if (sqlite3_step(q.s) != SQLITE_ROW) return false;
    out = Verification{};
    out.session_id = session_id; parse_pop_status(col_text(q.s, 0), out.status); out.reason = col_text(q.s, 1);
    out.expected_media_id = col_text(q.s, 2); out.best_media_id = col_text(q.s, 3); out.frames_total = sqlite3_column_int(q.s, 4);
    out.frames_ok = sqlite3_column_int(q.s, 5); out.frames_matched = sqlite3_column_int(q.s, 6); out.frames_mismatched = sqlite3_column_int(q.s, 7);
    out.mean_sim_expected = sqlite3_column_double(q.s, 8); out.min_sim_expected = sqlite3_column_double(q.s, 9); out.mean_phash_dist = sqlite3_column_double(q.s, 10);
    out.playback_reported = sqlite3_column_int(q.s, 11) != 0; out.playback_complete = sqlite3_column_int(q.s, 12) != 0;
    out.actual_play_ms = sqlite3_column_int64(q.s, 13); out.slot_ms = sqlite3_column_int64(q.s, 14); out.engine_version = col_text(q.s, 15);
    out.created_utc_ms = sqlite3_column_int64(q.s, 16); out.verification_id = sqlite3_column_int64(q.s, 17);
    out.frames_failed = std::max(0, out.frames_total - out.frames_ok);
    return true;
}
std::string PopStore::get_verification_json(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return {};
    Stmt q(impl_->db, "SELECT details_json FROM playback_verifications WHERE session_id=?;");
    if (!q) return {};
    q.text(1, session_id);
    return sqlite3_step(q.s) == SQLITE_ROW ? col_text(q.s, 0) : std::string{};
}


// ---- display-evidence search --------------------------------------------------------------------------------------
bool PopStore::get_evidence(int64_t evidence_id, Evidence& out) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    Stmt q(impl_->db, std::string("SELECT ") + (impl_->v2 ? kEvidenceCols : kEvidenceColsV1) + " FROM display_capture_evidence WHERE evidence_id=?;");
    if (!q) return false;
    sqlite3_bind_int64(q.s, 1, evidence_id);
    if (sqlite3_step(q.s) != SQLITE_ROW) return false;
    out = row_evidence(q.s);
    return true;
}

EvidenceCoverage PopStore::evidence_coverage(const EmbeddingSpec* model, const std::string& capture_method) {
    EvidenceCoverage c;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return c;
    c.rows_total = impl_->count("SELECT COUNT(*) FROM display_capture_evidence;");
    const int64_t mid = model ? impl_->model_id(*model, false) : 0;
    // capture_method is a schema-2 column; a schema-1 file only holds screenshots, so a resilience_grpc filter matches nothing there.
    const bool method_filter = !capture_method.empty();
    if (method_filter && !impl_->v2 && capture_method != "brightscript_screenshot") return c;
    const std::string where = std::string("status='ok' AND embedded=1") + (method_filter && impl_->v2 ? " AND capture_method=?" : "");
    {
        Stmt q(impl_->db, "SELECT COUNT(*) FROM display_capture_evidence WHERE " + where + (model ? " AND model_id<>?" : "") + ";");
        if (q) {
            int i = 1; if (method_filter && impl_->v2) q.text(i++, capture_method);
            if (model) sqlite3_bind_int64(q.s, i, mid);
            if (sqlite3_step(q.s) == SQLITE_ROW) c.rows_other_model = model ? static_cast<uint64_t>(sqlite3_column_int64(q.s, 0)) : 0;
        }
    }
    Stmt q(impl_->db, "SELECT COUNT(*),COALESCE(SUM(image_available),0),COALESCE(MIN(utc_ms),0),COALESCE(MAX(utc_ms),0),COUNT(DISTINCT session_id) "
                      "FROM display_capture_evidence WHERE " + where + (model ? " AND model_id=?" : "") + ";");
    if (!q) return c;
    int i = 1; if (method_filter && impl_->v2) q.text(i++, capture_method);
    if (model) sqlite3_bind_int64(q.s, i, mid);
    if (sqlite3_step(q.s) == SQLITE_ROW) {
        c.rows_searchable = static_cast<uint64_t>(sqlite3_column_int64(q.s, 0)); c.images_retained = static_cast<uint64_t>(sqlite3_column_int64(q.s, 1));
        c.oldest_utc_ms = sqlite3_column_int64(q.s, 2); c.newest_utc_ms = sqlite3_column_int64(q.s, 3); c.sessions = static_cast<uint64_t>(sqlite3_column_int64(q.s, 4));
    }
    c.checkpoint_rows = c.rows_searchable;
    if (impl_->hist) {
        c.rows_total += impl_->count("SELECT COUNT(*) FROM display_history_frames;");
        const bool hist_applies = !method_filter || capture_method == "resilience_grpc";
        const std::string hw = " FROM display_history_frames h WHERE h.status='ok' AND h.embedded=1 AND h.evidence_id IS NULL"
                               " AND NOT EXISTS (SELECT 1 FROM display_capture_evidence e WHERE e.session_id=h.session_id AND e.utc_ms=h.utc_ms AND e.status='ok' AND e.embedded=1 AND e.capture_method='resilience_grpc')";
        if (hist_applies && (!model || mid != 0)) {
            const std::string mclause = model ? " AND h.model_id=?" : "";
            Stmt hq(impl_->db, "SELECT COUNT(*),COALESCE(SUM(h.image_available),0),COALESCE(MIN(h.utc_ms),0),COALESCE(MAX(h.utc_ms),0)" + hw + mclause + ";");
            if (hq) {
                if (model) sqlite3_bind_int64(hq.s, 1, mid);
                if (sqlite3_step(hq.s) == SQLITE_ROW) {
                    const uint64_t n = static_cast<uint64_t>(sqlite3_column_int64(hq.s, 0));
                    c.history_rows = n; c.rows_searchable += n; c.images_retained += static_cast<uint64_t>(sqlite3_column_int64(hq.s, 1));
                    if (n) {
                        const int64_t lo = sqlite3_column_int64(hq.s, 2), hi = sqlite3_column_int64(hq.s, 3);
                        c.oldest_utc_ms = c.oldest_utc_ms ? std::min(c.oldest_utc_ms, lo) : lo; c.newest_utc_ms = std::max(c.newest_utc_ms, hi);
                    }
                }
            }
            Stmt sq(impl_->db, "SELECT COUNT(*) FROM (SELECT session_id FROM display_capture_evidence WHERE " + where + (model ? " AND model_id=?" : "") + " UNION SELECT h.session_id" + hw + mclause + ");");
            if (sq) {
                int i = 1; if (method_filter && impl_->v2) sq.text(i++, capture_method);
                if (model) { sqlite3_bind_int64(sq.s, i++, mid); sqlite3_bind_int64(sq.s, i, mid); }
                if (sqlite3_step(sq.s) == SQLITE_ROW) c.sessions = static_cast<uint64_t>(sqlite3_column_int64(sq.s, 0));
            }
        }
    }
    return c;
}

// ---- display history (Phase 2) ----------------------------------------------------------------------------------
bool PopStore::history_open(std::string& err) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) { err = "store not open"; return false; }
    if (!impl_->v2) { err = "pop.db is schema 1: display history needs visual_memory.pop.allow_schema_upgrade"; return false; }
    auto time_indexes = [&] {   // additive, optional: they only speed up the time lookup (pop.db user_version stays 2)
        std::string e2;
        impl_->exec("CREATE INDEX IF NOT EXISTS ix_pop_sess_start ON playback_sessions(start_utc_ms);"
                    "CREATE INDEX IF NOT EXISTS ix_pop_hist_evidence ON display_history_frames(evidence_id) WHERE evidence_id IS NOT NULL;", e2);
    };
    if (impl_->hist) { time_indexes(); return true; }
    const char* ddl =
        "CREATE TABLE IF NOT EXISTS display_history_frames(frame_id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,sec_idx INTEGER NOT NULL,"
        " evidence_id INTEGER,source TEXT NOT NULL,expected_media_id TEXT NOT NULL DEFAULT '',played_media_id TEXT NOT NULL DEFAULT '',"
        " utc_ms INTEGER NOT NULL,mono_ms INTEGER NOT NULL DEFAULT 0,target_offset_ms INTEGER NOT NULL DEFAULT 0,actual_offset_ms INTEGER NOT NULL DEFAULT 0,"
        " uncertainty_ms INTEGER NOT NULL DEFAULT -1,capture_method TEXT NOT NULL DEFAULT 'resilience_grpc',provenance TEXT NOT NULL DEFAULT '',"
        " path TEXT NOT NULL DEFAULT '',sha256 TEXT NOT NULL DEFAULT '',width INTEGER NOT NULL DEFAULT 0,height INTEGER NOT NULL DEFAULT 0,"
        " bytes INTEGER NOT NULL DEFAULT 0,model_id INTEGER NOT NULL DEFAULT 0,embedding BLOB,embedded INTEGER NOT NULL DEFAULT 0,"
        " embed_attempts INTEGER NOT NULL DEFAULT 0,image_available INTEGER NOT NULL DEFAULT 0,status TEXT NOT NULL DEFAULT 'ok',created_at INTEGER NOT NULL,"
        " UNIQUE(session_id,sec_idx));"
        "CREATE INDEX IF NOT EXISTS ix_pop_hist_time ON display_history_frames(utc_ms);"
        "CREATE INDEX IF NOT EXISTS ix_pop_hist_pending ON display_history_frames(embedded,embed_attempts);";
    if (!impl_->exec("BEGIN IMMEDIATE;", err)) return false;
    if (!impl_->exec(ddl, err)) { std::string e2; impl_->exec("ROLLBACK;", e2); return false; }
    if (!impl_->exec("COMMIT;", err)) return false;
    impl_->hist = true;
    time_indexes();
    return true;
}
bool PopStore::history_ready() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->db && impl_->hist; }

bool PopStore::add_history_frame(const HistoryFrameIn& in, const HistoryLimits& lim, HistoryFrame& out, std::string& err) {
    out = HistoryFrame{};
    if (!valid_id(in.session_id)) { err = "invalid session_id"; return false; }
    if (in.sec_idx < 0 || in.sec_idx > 1000000) { err = "sec_idx out of range"; return false; }
    int w = 0, h = 0;
    if (in.jpeg.size() > kMaxJpegBytes || !jpeg_dims(in.jpeg, w, h)) { err = "not a complete JPEG (SOI..EOI with a frame header) within 8 MiB"; return false; }
    out.session_id = in.session_id; out.sec_idx = in.sec_idx; out.source = in.source.empty() ? "main_display" : in.source;
    out.expected_media_id = in.expected_media_id.substr(0, 64); out.utc_ms = in.utc_ms ? in.utc_ms : now_ms(); out.mono_ms = in.mono_ms;
    out.target_offset_ms = in.target_offset_ms; out.actual_offset_ms = in.actual_offset_ms; out.uncertainty_ms = in.uncertainty_ms;
    out.capture_method = in.capture_method.substr(0, 40); out.provenance = in.provenance.substr(0, 120);
    out.width = w; out.height = h; out.bytes = in.jpeg.size();
    int64_t link = 0, link_model = 0; Evidence ck;
    {   // admission checks and the checkpoint-reuse decision
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->db || !impl_->hist) { err = "display history is not enabled"; return false; }
        if (!impl_->session_exists(in.session_id)) { err = "unknown session"; return false; }
        {
            Stmt q(impl_->db, "SELECT 1 FROM display_history_frames WHERE session_id=? AND sec_idx=?;");
            if (!q) { err = sqlite3_errmsg(impl_->db); return false; }
            q.text(1, in.session_id); sqlite3_bind_int(q.s, 2, in.sec_idx);
            if (sqlite3_step(q.s) == SQLITE_ROW) { err = "duplicate"; return false; }
        }
        {
            Stmt q(impl_->db, "SELECT COUNT(*) FROM display_history_frames WHERE session_id=?;");
            if (q) { q.text(1, in.session_id); if (sqlite3_step(q.s) == SQLITE_ROW && sqlite3_column_int(q.s, 0) >= lim.max_frames_per_session) { err = "session limit"; return false; } }
        }
        if (impl_->count("SELECT COUNT(*) FROM display_history_frames WHERE embedded=0 AND status='ok' AND evidence_id IS NULL AND embed_attempts<3;") >= lim.max_pending_embeddings) {
            err = "embedding backlog"; return false;
        }
        Stmt q(impl_->db, "SELECT evidence_id,model_id,sha256 FROM display_capture_evidence WHERE session_id=? AND utc_ms=? AND status='ok' AND embedded=1 AND capture_method='resilience_grpc' LIMIT 1;");
        if (q) {
            q.text(1, in.session_id); sqlite3_bind_int64(q.s, 2, out.utc_ms);
            if (sqlite3_step(q.s) == SQLITE_ROW) { link = sqlite3_column_int64(q.s, 0); link_model = sqlite3_column_int64(q.s, 1); out.sha256 = col_text(q.s, 2); }
        }
    }
    if (!link) {
        out.sha256 = detail::sha256_hex(in.jpeg.data(), in.jpeg.size());
        out.path = history_path(out.source, out.utc_ms, in.session_id + "_h" + std::to_string(in.sec_idx) + ".jpg");
        if (!write_atomic(fs::path(impl_->cfg.root_directory) / out.path, in.jpeg, err)) return false;
        out.image_available = true;
    } else { out.evidence_id = link; out.embedded = true; out.role = "history_and_checkpoint"; }
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) { err = "store not open"; return false; }
    Stmt q(impl_->db, "INSERT INTO display_history_frames(session_id,sec_idx,evidence_id,source,expected_media_id,utc_ms,mono_ms,target_offset_ms,actual_offset_ms,"
                      "uncertainty_ms,capture_method,provenance,path,sha256,width,height,bytes,model_id,embedded,image_available,created_at) "
                      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!q) { err = sqlite3_errmsg(impl_->db); return false; }
    q.text(1, out.session_id); sqlite3_bind_int(q.s, 2, out.sec_idx);
    if (link) sqlite3_bind_int64(q.s, 3, link); else sqlite3_bind_null(q.s, 3);
    q.text(4, out.source); q.text(5, out.expected_media_id); sqlite3_bind_int64(q.s, 6, out.utc_ms); sqlite3_bind_int64(q.s, 7, out.mono_ms);
    sqlite3_bind_int(q.s, 8, out.target_offset_ms); sqlite3_bind_int(q.s, 9, out.actual_offset_ms); sqlite3_bind_int(q.s, 10, out.uncertainty_ms);
    q.text(11, out.capture_method); q.text(12, out.provenance); q.text(13, out.path); q.text(14, out.sha256); sqlite3_bind_int(q.s, 15, out.width);
    sqlite3_bind_int(q.s, 16, out.height); sqlite3_bind_int64(q.s, 17, static_cast<int64_t>(out.bytes)); sqlite3_bind_int64(q.s, 18, link_model);
    sqlite3_bind_int(q.s, 19, out.embedded ? 1 : 0); sqlite3_bind_int(q.s, 20, out.image_available ? 1 : 0); sqlite3_bind_int64(q.s, 21, now_ms());
    if (sqlite3_step(q.s) != SQLITE_DONE) {
        err = sqlite3_extended_errcode(impl_->db) == SQLITE_CONSTRAINT_UNIQUE || sqlite3_errcode(impl_->db) == SQLITE_CONSTRAINT ? "duplicate" : sqlite3_errmsg(impl_->db);
        std::error_code ec; if (!out.path.empty()) fs::remove(fs::path(impl_->cfg.root_directory) / out.path, ec);
        return false;
    }
    out.frame_id = sqlite3_last_insert_rowid(impl_->db);
    return true;
}

int PopStore::embed_pending_history(EmbeddingProvider& provider, int max_rows, std::string& err) {
    struct Job { int64_t id; std::string path; };
    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->db || !impl_->hist) return 0;
        Stmt q(impl_->db, "SELECT frame_id,path FROM display_history_frames WHERE embedded=0 AND status='ok' AND evidence_id IS NULL AND embed_attempts<3 "
                          "ORDER BY frame_id LIMIT ?;");
        if (!q) { err = sqlite3_errmsg(impl_->db); return -1; }
        sqlite3_bind_int(q.s, 1, std::max(1, max_rows));
        while (sqlite3_step(q.s) == SQLITE_ROW) jobs.push_back({sqlite3_column_int64(q.s, 0), col_text(q.s, 1)});
    }
    int done = 0;
    for (const auto& j : jobs) {
        std::vector<uint8_t> jpeg; std::vector<float> emb; int w = 0, h = 0; std::string e;
        bool ok = detail::read_file_bounded((fs::path(impl_->cfg.root_directory) / j.path).string(), kMaxJpegBytes, jpeg, e) && decode_embed(provider, jpeg, emb, w, h, e);
        const bool missing = !ok && !fs::exists(fs::path(impl_->cfg.root_directory) / j.path);
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->db) return done;
        if (ok) {
            const int64_t mid = impl_->model_id(provider.spec(), true);
            if (mid == 0) { err = "cannot register model"; return -1; }
            Stmt u(impl_->db, "UPDATE display_history_frames SET model_id=?,embedding=?,embedded=1 WHERE frame_id=?;");
            if (!u) { err = sqlite3_errmsg(impl_->db); return -1; }
            sqlite3_bind_int64(u.s, 1, mid); bind_vec(u.s, 2, emb); sqlite3_bind_int64(u.s, 3, j.id);
            if (sqlite3_step(u.s) != SQLITE_DONE) { err = sqlite3_errmsg(impl_->db); return -1; }
            ++done;
        } else {
            Stmt u(impl_->db, missing ? "UPDATE display_history_frames SET embed_attempts=3 WHERE frame_id=?;" : "UPDATE display_history_frames SET embed_attempts=embed_attempts+1 WHERE frame_id=?;");
            if (u) { sqlite3_bind_int64(u.s, 1, j.id); sqlite3_step(u.s); }
            err = e;
        }
    }
    return done;
}
uint64_t PopStore::history_pending_embeddings() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return (impl_->db && impl_->hist) ? impl_->count("SELECT COUNT(*) FROM display_history_frames WHERE embedded=0 AND status='ok' AND evidence_id IS NULL AND embed_attempts<3;") : 0;
}
bool PopStore::get_history(int64_t frame_id, HistoryFrame& out) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db || !impl_->hist) return false;
    Stmt q(impl_->db, std::string("SELECT ") + kHistCols + " FROM display_history_frames WHERE frame_id=?;");
    if (!q) return false;
    sqlite3_bind_int64(q.s, 1, frame_id);
    if (sqlite3_step(q.s) != SQLITE_ROW) return false;
    out = row_hist(q.s);
    return true;
}
std::vector<HistoryFrame> PopStore::list_history(const std::string& session_id, int limit, int offset, uint64_t* total) {
    std::vector<HistoryFrame> v;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db || !impl_->hist) { if (total) *total = 0; return v; }
    const std::string where = session_id.empty() ? "" : " WHERE session_id=?";
    if (total) {
        Stmt c(impl_->db, "SELECT COUNT(*) FROM display_history_frames" + where + ";");
        if (c) { if (!session_id.empty()) c.text(1, session_id); *total = sqlite3_step(c.s) == SQLITE_ROW ? static_cast<uint64_t>(sqlite3_column_int64(c.s, 0)) : 0; }
    }
    Stmt q(impl_->db, std::string("SELECT ") + kHistCols + " FROM display_history_frames" + where + " ORDER BY session_id,sec_idx LIMIT ? OFFSET ?;");
    if (!q) return v;
    int i = 1; if (!session_id.empty()) q.text(i++, session_id);
    sqlite3_bind_int(q.s, i++, std::max(1, std::min(1000, limit))); sqlite3_bind_int(q.s, i, std::max(0, offset));
    while (sqlite3_step(q.s) == SQLITE_ROW) v.push_back(row_hist(q.s));
    return v;
}
bool PopStore::history_jpeg(int64_t frame_id, std::vector<uint8_t>& out) {
    HistoryFrame h;
    if (!get_history(frame_id, h)) return false;
    if (h.evidence_id) return evidence_jpeg(h.evidence_id, out);
    if (!h.image_available || h.path.empty()) return false;
    std::string err;
    return detail::read_file_bounded((fs::path(impl_->cfg.root_directory) / h.path).string(), kMaxJpegBytes, out, err);
}
HistoryStats PopStore::history_stats() {
    HistoryStats s;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db || !impl_->hist) return s;
    s.ready = true;
    s.rows = impl_->count("SELECT COUNT(*) FROM display_history_frames;");
    s.embedded = impl_->count("SELECT COUNT(*) FROM display_history_frames WHERE embedded=1;");
    s.pending_embed = impl_->count("SELECT COUNT(*) FROM display_history_frames WHERE embedded=0 AND status='ok' AND evidence_id IS NULL AND embed_attempts<3;");
    s.embed_failed = impl_->count("SELECT COUNT(*) FROM display_history_frames WHERE embedded=0 AND embed_attempts>=3;");
    s.linked_to_checkpoint = impl_->count("SELECT COUNT(*) FROM display_history_frames WHERE evidence_id IS NOT NULL;");
    s.images_retained = impl_->count("SELECT COUNT(*) FROM display_history_frames WHERE image_available=1;");
    s.image_bytes = impl_->count("SELECT COALESCE(SUM(bytes),0) FROM display_history_frames WHERE image_available=1;");
    s.sessions = impl_->count("SELECT COUNT(DISTINCT session_id) FROM display_history_frames;");
    Stmt q(impl_->db, "SELECT COALESCE(MIN(utc_ms),0),COALESCE(MAX(utc_ms),0) FROM display_history_frames;");
    if (q && sqlite3_step(q.s) == SQLITE_ROW) { s.oldest_utc_ms = sqlite3_column_int64(q.s, 0); s.newest_utc_ms = sqlite3_column_int64(q.s, 1); }
    return s;
}
// ---- time lookup (read-only) -------------------------------------------------------------------------------------------------------
namespace {
// Column order shared by both sources: kind(0 hist,1 evidence),frame_id,evidence_id,index,session,source,method,provenance,expected,utc,offset,uncertainty,w,h,bytes,sha,embedded,image
const char* kDispHistSel =
    "SELECT 0,h.frame_id,COALESCE(h.evidence_id,0),h.sec_idx,h.session_id,h.source,h.capture_method,h.provenance,h.expected_media_id,h.utc_ms,h.actual_offset_ms,"
    "h.uncertainty_ms,h.width,h.height,h.bytes,h.sha256,"
    "CASE WHEN h.evidence_id IS NULL THEN h.embedded ELSE COALESCE((SELECT e.embedded FROM display_capture_evidence e WHERE e.evidence_id=h.evidence_id),0) END,"
    "CASE WHEN h.evidence_id IS NULL THEN h.image_available ELSE COALESCE((SELECT e.image_available FROM display_capture_evidence e WHERE e.evidence_id=h.evidence_id),0) END"
    " FROM display_history_frames h WHERE h.status='ok'";
const char* kDispEvSel =
    "SELECT 1,0,e.evidence_id,e.idx,e.session_id,e.source,e.capture_method,e.provenance,"
    "COALESCE((SELECT s.expected_media_id FROM playback_sessions s WHERE s.session_id=e.session_id),''),e.utc_ms,e.actual_offset_ms,e.uncertainty_ms,e.width,e.height,"
    "e.bytes,e.sha256,e.embedded,e.image_available FROM display_capture_evidence e WHERE e.status='ok' AND e.capture_method='resilience_grpc'";

DisplayFrameRef row_dfr(sqlite3_stmt* s) {
    DisplayFrameRef f;
    const bool ev = sqlite3_column_int(s, 0) == 1;
    f.frame_id = sqlite3_column_int64(s, 1); f.evidence_id = sqlite3_column_int64(s, 2); f.index = sqlite3_column_int(s, 3);
    f.kind = ev ? "verification_checkpoint" : (f.evidence_id ? "history_and_checkpoint" : "history_sample");
    f.session_id = col_text(s, 4); f.source = col_text(s, 5); f.capture_method = col_text(s, 6); f.provenance = col_text(s, 7); f.expected_media_id = col_text(s, 8);
    f.utc_ms = sqlite3_column_int64(s, 9); f.actual_offset_ms = sqlite3_column_int(s, 10); f.uncertainty_ms = sqlite3_column_int(s, 11);
    f.width = sqlite3_column_int(s, 12); f.height = sqlite3_column_int(s, 13); f.bytes = static_cast<uint64_t>(sqlite3_column_int64(s, 14)); f.sha256 = col_text(s, 15);
    f.embedded = sqlite3_column_int(s, 16) != 0; f.image_available = sqlite3_column_int(s, 17) != 0;
    return f;
}

// Nearest frame on one side of t in one source. Two indexed probes (the best utc_ms, then the lowest id at that instant) keep the
// choice deterministic without a sort over the table: ties at the same instant resolve to the lowest id.
bool probe_side(sqlite3* db, bool hist_src, bool has_hist, int64_t t, bool before, const std::string& sid, DisplayFrameRef& out) {
    const char* tc = hist_src ? "h.utc_ms" : "e.utc_ms"; const char* idc = hist_src ? "h.frame_id" : "e.evidence_id";
    std::string where = std::string(" AND ") + tc + (before ? "<=?" : ">?");
    if (!sid.empty()) where += hist_src ? " AND h.session_id=?" : " AND e.session_id=?";
    if (!hist_src && has_hist) where += " AND NOT EXISTS(SELECT 1 FROM display_history_frames x WHERE x.evidence_id=e.evidence_id)";
    const std::string base = hist_src ? kDispHistSel : kDispEvSel;
    int64_t best = 0;
    {
        Stmt q(db, std::string(hist_src ? "SELECT h.utc_ms FROM display_history_frames h WHERE h.status='ok'" : "SELECT e.utc_ms FROM display_capture_evidence e WHERE e.status='ok' AND e.capture_method='resilience_grpc'") +
                       where + " ORDER BY " + tc + (before ? " DESC" : " ASC") + " LIMIT 1;");
        if (!q) return false;
        int i = 1; sqlite3_bind_int64(q.s, i++, t); if (!sid.empty()) q.text(i++, sid);
        if (sqlite3_step(q.s) != SQLITE_ROW) return false;
        best = sqlite3_column_int64(q.s, 0);
    }
    Stmt q(db, base + std::string(" AND ") + tc + "=?" + (sid.empty() ? "" : (hist_src ? " AND h.session_id=?" : " AND e.session_id=?")) +
                   ((!hist_src && has_hist) ? " AND NOT EXISTS(SELECT 1 FROM display_history_frames x WHERE x.evidence_id=e.evidence_id)" : "") +
                   " ORDER BY " + idc + " ASC LIMIT 1;");
    if (!q) return false;
    int i = 1; sqlite3_bind_int64(q.s, i++, best); if (!sid.empty()) q.text(i++, sid);
    if (sqlite3_step(q.s) != SQLITE_ROW) return false;
    out = row_dfr(q.s);
    return true;
}
}  // namespace

bool PopStore::nearest_display_frame(int64_t utc_ms, int64_t max_diff_ms, const std::string& session_id, NearestDisplayFrame& out) {
    out = NearestDisplayFrame{};
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db || !impl_->v2) return false;
    const bool hh = impl_->hist;
    std::vector<DisplayFrameRef> c;
    for (int src = 0; src < 2; ++src) {                      // 0 = history rows, 1 = checkpoints that history does not already link
        if (src == 0 && !hh) continue;
        for (int side = 0; side < 2; ++side) {               // side 0 = at or before the requested time, 1 = after it
            DisplayFrameRef f;
            if (probe_side(impl_->db, src == 0, hh, utc_ms, side == 0, session_id, f)) c.push_back(std::move(f));
        }
    }
    // order: smallest |diff|, then the earlier frame, then history before checkpoint, then lowest id
    auto key = [&](const DisplayFrameRef& f) {
        const int64_t d = f.utc_ms - utc_ms;
        return std::make_tuple(d < 0 ? -d : d, d > 0 ? 1 : 0, f.kind == "verification_checkpoint" ? 1 : 0, f.frame_id ? f.frame_id : f.evidence_id);
    };
    if (!c.empty()) {
        const auto it = std::min_element(c.begin(), c.end(), [&](const DisplayFrameRef& a, const DisplayFrameRef& b) { return key(a) < key(b); });
        out.frame = *it; out.has_nearest = true; out.diff_ms = it->utc_ms - utc_ms;
        const int64_t ad = out.diff_ms < 0 ? -out.diff_ms : out.diff_ms;
        out.found = ad <= std::max<int64_t>(0, max_diff_ms);
    }
    // coverage of what is retained at all (all sessions), so a miss can be explained
    out.frames_in_store = impl_->count(hh ? "SELECT COUNT(*) FROM display_capture_evidence e WHERE e.status='ok' AND e.capture_method='resilience_grpc' AND NOT EXISTS(SELECT 1 FROM display_history_frames x WHERE x.evidence_id=e.evidence_id);"
                                       : "SELECT COUNT(*) FROM display_capture_evidence e WHERE e.status='ok' AND e.capture_method='resilience_grpc';");
    int64_t lo = 0, hi = 0; bool any = false;
    auto span = [&](const char* sql) {
        Stmt q(impl_->db, sql);
        if (q && sqlite3_step(q.s) == SQLITE_ROW && sqlite3_column_type(q.s, 0) != SQLITE_NULL) {
            const int64_t a = sqlite3_column_int64(q.s, 0), b = sqlite3_column_int64(q.s, 1);
            lo = any ? std::min(lo, a) : a; hi = any ? std::max(hi, b) : b; any = true;
        }
    };
    span("SELECT MIN(utc_ms),MAX(utc_ms) FROM display_capture_evidence WHERE status='ok' AND capture_method='resilience_grpc';");
    if (hh) {
        out.frames_in_store += impl_->count("SELECT COUNT(*) FROM display_history_frames WHERE status='ok';");
        span("SELECT MIN(utc_ms),MAX(utc_ms) FROM display_history_frames WHERE status='ok';");
    }
    if (any) { out.oldest_utc_ms = lo; out.newest_utc_ms = hi; }
    return true;
}

std::vector<DisplayFrameRef> PopStore::display_frames_between(int64_t from_utc_ms, int64_t to_utc_ms, const std::string& session_id, int limit, bool* truncated, bool newest_first) {
    std::vector<DisplayFrameRef> v;
    if (truncated) *truncated = false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db || !impl_->v2) return v;
    limit = std::clamp(limit, 1, 5000);
    const bool hh = impl_->hist;
    auto run = [&](bool hist_src) {
        const char* tc = hist_src ? "h.utc_ms" : "e.utc_ms";
        std::string sql = std::string(hist_src ? kDispHistSel : kDispEvSel) + " AND " + tc + ">=? AND " + tc + "<=?";
        if (!session_id.empty()) sql += hist_src ? " AND h.session_id=?" : " AND e.session_id=?";
        if (!hist_src && hh) sql += " AND NOT EXISTS(SELECT 1 FROM display_history_frames x WHERE x.evidence_id=e.evidence_id)";
        sql += std::string(" ORDER BY ") + tc + (newest_first ? " DESC" : "") + (hist_src ? ",h.frame_id" : ",e.evidence_id") + (newest_first ? " DESC" : "") + " LIMIT ?;";
        Stmt q(impl_->db, sql);
        if (!q) return;
        int i = 1; sqlite3_bind_int64(q.s, i++, from_utc_ms); sqlite3_bind_int64(q.s, i++, to_utc_ms); if (!session_id.empty()) q.text(i++, session_id);
        sqlite3_bind_int(q.s, i, limit + 1);
        while (sqlite3_step(q.s) == SQLITE_ROW) v.push_back(row_dfr(q.s));
    };
    if (hh) run(true);
    run(false);
    std::sort(v.begin(), v.end(), [newest_first](const DisplayFrameRef& a, const DisplayFrameRef& b) {
        if (a.utc_ms != b.utc_ms) return newest_first ? a.utc_ms > b.utc_ms : a.utc_ms < b.utc_ms;
        if (a.kind == "verification_checkpoint" != (b.kind == "verification_checkpoint")) return a.kind != "verification_checkpoint";
        const int64_t ia = a.frame_id ? a.frame_id : a.evidence_id, ib = b.frame_id ? b.frame_id : b.evidence_id;
        return newest_first ? ia > ib : ia < ib;
    });
    if (static_cast<int>(v.size()) > limit) { v.resize(limit); if (truncated) *truncated = true; }
    return v;
}

std::vector<FrameSim> PopStore::expected_similarity(const std::string& session_id, const std::vector<FrameSimQuery>& qs) {
    std::vector<FrameSim> out(qs.size());
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto fail = [&](const std::string& why) { for (auto& o : out) o.reason = why; return out; };
    if (!impl_->db || !impl_->v2) return fail("store unavailable");
    PopSession s;
    if (!impl_->load_session(session_id, s)) return fail("unknown session");
    if (s.expected_media_id.empty()) return fail("the session has no expected media");
    struct Ref { std::string media; int off = 0; std::vector<float> e; };
    std::map<int64_t, std::vector<Ref>> refs_by_model;
    const std::string suffix = kDisplayProfileSuffix();
    auto refs_for = [&](int64_t model) -> const std::vector<Ref>& {
        auto it = refs_by_model.find(model);
        if (it != refs_by_model.end()) return it->second;
        std::vector<Ref>& v = refs_by_model[model];
        Stmt q(impl_->db, "SELECT media_id,offset_ms,embedding FROM media_reference_frames WHERE model_id=? AND media_id IN (?,?);");
        if (q) {
            sqlite3_bind_int64(q.s, 1, model); q.text(2, s.expected_media_id); q.text(3, s.expected_media_id + suffix);
            while (sqlite3_step(q.s) == SQLITE_ROW) { Ref r; r.media = col_text(q.s, 0); r.off = sqlite3_column_int(q.s, 1); r.e = blob_vec(q.s, 2); v.push_back(std::move(r)); }
        }
        return v;
    };
    for (size_t k = 0; k < qs.size(); ++k) {
        const bool ev = qs[k].evidence_id != 0;
        if (!ev && qs[k].frame_id == 0) { out[k].reason = "no frame"; continue; }
        Stmt q(impl_->db, ev ? "SELECT model_id,embedding,embedded,capture_method FROM display_capture_evidence WHERE evidence_id=?;"
                             : "SELECT model_id,embedding,embedded,capture_method FROM display_history_frames WHERE frame_id=?;");
        if (!q) { out[k].reason = "query failed"; continue; }
        sqlite3_bind_int64(q.s, 1, ev ? qs[k].evidence_id : qs[k].frame_id);
        if (sqlite3_step(q.s) != SQLITE_ROW) { out[k].reason = "frame not found"; continue; }
        if (!sqlite3_column_int(q.s, 2) || sqlite3_column_type(q.s, 1) == SQLITE_NULL) { out[k].reason = "the frame's vector has not been computed (yet)"; continue; }
        const int64_t model = sqlite3_column_int64(q.s, 0);
        const std::vector<float> fv = blob_vec(q.s, 1);
        const bool weston = col_text(q.s, 3) == "resilience_grpc";
        const auto& all = refs_for(model);
        bool have_display = false;
        for (const auto& r : all) if (r.media == s.expected_media_id + suffix) have_display = true;
        const std::string want = (weston && have_display) ? s.expected_media_id + suffix : s.expected_media_id;     // same rule as the verifier: never mix the sets
        float best = -2.0f; int boff = -1, n = 0;
        for (const auto& r : all) {
            if (r.media != want || r.e.size() != fv.size() || fv.empty()) continue;
            float d = 0; for (size_t i = 0; i < fv.size(); ++i) d += fv[i] * r.e[i];
            ++n; if (d > best) { best = d; boff = r.off; }
        }
        if (!n) { out[k].reason = "no reference frames of the expected media for this model"; continue; }
        out[k].ok = true; out[k].sim = best; out[k].best_ref_offset_ms = boff; out[k].refs = n;
    }
    return out;
}

std::vector<PopSession> PopStore::list_sessions_overlapping(int64_t from_utc_ms, int64_t to_utc_ms, int limit) {
    std::vector<PopSession> v;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return v;
    std::string cols;
    for (const char* p = kSessionCols; *p; ++p) { if (p == kSessionCols || *(p - 1) == ',') cols += "s."; cols += *p; }
    Stmt q(impl_->db, "SELECT " + cols + " FROM playback_sessions s WHERE (s.start_utc_ms>0 AND s.start_utc_ms<=? AND "
                      "(CASE WHEN s.end_utc_ms>0 THEN s.end_utc_ms ELSE s.start_utc_ms+s.slot_ms END)>=?) OR "
                      "(s.start_utc_ms=0 AND s.scheduled_utc_ms>=? AND s.scheduled_utc_ms<=?) ORDER BY s.start_utc_ms,s.session_id LIMIT ?;");
    if (!q) return v;
    sqlite3_bind_int64(q.s, 1, to_utc_ms); sqlite3_bind_int64(q.s, 2, from_utc_ms); sqlite3_bind_int64(q.s, 3, from_utc_ms); sqlite3_bind_int64(q.s, 4, to_utc_ms);
    sqlite3_bind_int(q.s, 5, std::clamp(limit, 1, 500));
    while (sqlite3_step(q.s) == SQLITE_ROW) v.push_back(row_session(q.s));
    return v;
}


uint64_t PopStore::history_finalize_session(const std::string& session_id, int64_t end_utc_ms, int guard_ms, const std::string& played_media_id) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db || !impl_->hist) return 0;
    std::vector<std::string> files;
    uint64_t removed = 0;
    if (end_utc_ms > 0) {
        Stmt q(impl_->db, "SELECT path FROM display_history_frames WHERE session_id=? AND utc_ms>?;");
        if (q) { q.text(1, session_id); sqlite3_bind_int64(q.s, 2, end_utc_ms - guard_ms); while (sqlite3_step(q.s) == SQLITE_ROW) { const std::string pth = col_text(q.s, 0); if (!pth.empty()) files.push_back(pth); } }
        Stmt d(impl_->db, "DELETE FROM display_history_frames WHERE session_id=? AND utc_ms>?;");
        if (d) { d.text(1, session_id); sqlite3_bind_int64(d.s, 2, end_utc_ms - guard_ms); if (sqlite3_step(d.s) == SQLITE_DONE) removed = static_cast<uint64_t>(sqlite3_changes(impl_->db)); }
        for (const auto& f : files) { std::error_code ec; fs::remove(fs::path(impl_->cfg.root_directory) / f, ec); }
    }
    if (!played_media_id.empty()) {
        Stmt u(impl_->db, "UPDATE display_history_frames SET played_media_id=? WHERE session_id=?;");
        if (u) { u.text(1, played_media_id); u.text(2, session_id); sqlite3_step(u.s); }
    }
    return removed;
}
HistoryPruneResult PopStore::prune_history(int64_t jpeg_older_than_ms, uint64_t max_bytes, int64_t meta_older_than_ms, uint64_t max_rows) {
    HistoryPruneResult r;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db || !impl_->hist) return r;
    struct Row { int64_t id; std::string path; uint64_t bytes; int64_t utc; };
    std::vector<Row> rows; uint64_t total = 0;
    {
        Stmt q(impl_->db, "SELECT frame_id,path,bytes,utc_ms FROM display_history_frames WHERE image_available=1 ORDER BY utc_ms,frame_id;");
        while (q && sqlite3_step(q.s) == SQLITE_ROW) { rows.push_back({sqlite3_column_int64(q.s, 0), col_text(q.s, 1), static_cast<uint64_t>(sqlite3_column_int64(q.s, 2)), sqlite3_column_int64(q.s, 3)}); total += rows.back().bytes; }
    }
    for (const auto& w : rows) {
        const bool old = jpeg_older_than_ms > 0 && w.utc < jpeg_older_than_ms;
        const bool over = max_bytes > 0 && total > max_bytes;
        if (!old && !over) break;
        std::error_code ec; fs::remove(fs::path(impl_->cfg.root_directory) / w.path, ec);
        Stmt u(impl_->db, "UPDATE display_history_frames SET image_available=0 WHERE frame_id=?;");
        if (u) { sqlite3_bind_int64(u.s, 1, w.id); sqlite3_step(u.s); }
        total -= w.bytes; r.bytes_freed += w.bytes; ++r.images_removed;
    }
    if (meta_older_than_ms > 0) {
        std::vector<std::string> files;
        Stmt q(impl_->db, "SELECT path FROM display_history_frames WHERE utc_ms<? AND image_available=1;");
        if (q) { sqlite3_bind_int64(q.s, 1, meta_older_than_ms); while (sqlite3_step(q.s) == SQLITE_ROW) files.push_back(col_text(q.s, 0)); }
        for (const auto& f : files) { std::error_code ec; if (!f.empty()) fs::remove(fs::path(impl_->cfg.root_directory) / f, ec); }
        Stmt d(impl_->db, "DELETE FROM display_history_frames WHERE utc_ms<?;");
        if (d) { sqlite3_bind_int64(d.s, 1, meta_older_than_ms); if (sqlite3_step(d.s) == SQLITE_DONE) r.rows_removed = static_cast<uint64_t>(sqlite3_changes(impl_->db)); }
    }
    if (max_rows > 0) {                                                          // bound the exact-scan size: the oldest rows (and their files) go first
        const uint64_t n = impl_->count("SELECT COUNT(*) FROM display_history_frames;");
        if (n > max_rows) {
            const uint64_t drop = n - max_rows;
            std::vector<std::string> files;
            Stmt q(impl_->db, "SELECT path FROM display_history_frames WHERE image_available=1 AND frame_id IN (SELECT frame_id FROM display_history_frames ORDER BY utc_ms,frame_id LIMIT ?);");
            if (q) { sqlite3_bind_int64(q.s, 1, static_cast<int64_t>(drop)); while (sqlite3_step(q.s) == SQLITE_ROW) files.push_back(col_text(q.s, 0)); }
            for (const auto& f : files) { std::error_code ec; if (!f.empty()) fs::remove(fs::path(impl_->cfg.root_directory) / f, ec); }
            Stmt d(impl_->db, "DELETE FROM display_history_frames WHERE frame_id IN (SELECT frame_id FROM display_history_frames ORDER BY utc_ms,frame_id LIMIT ?);");
            if (d) { sqlite3_bind_int64(d.s, 1, static_cast<int64_t>(drop)); if (sqlite3_step(d.s) == SQLITE_DONE) r.rows_removed += static_cast<uint64_t>(sqlite3_changes(impl_->db)); }
        }
    }
    return r;
}

// Fractional-box area resample of the source rectangle [sx0,sx1)x[sy0,sy1) onto dw x dh (a box smaller than a pixel is widened to one pixel).
static void area_resample_rgb(const uint8_t* src, int sw, int sh, double sx0, double sy0, double sx1, double sy1, uint8_t* dst, int dw, int dh, int dstride, int dx0) {
    const double bw = (sx1 - sx0) / dw, bh = (sy1 - sy0) / dh;
    for (int y = 0; y < dh; ++y) {
        double a0 = sy0 + y * bh, a1 = a0 + bh;
        if (a1 - a0 < 1.0) { const double c = (a0 + a1) / 2; a0 = c - 0.5; a1 = c + 0.5; }
        a0 = std::max(a0, 0.0); a1 = std::min(a1, static_cast<double>(sh));
        for (int x = 0; x < dw; ++x) {
            double b0 = sx0 + x * bw, b1 = b0 + bw;
            if (b1 - b0 < 1.0) { const double c = (b0 + b1) / 2; b0 = c - 0.5; b1 = c + 0.5; }
            b0 = std::max(b0, 0.0); b1 = std::min(b1, static_cast<double>(sw));
            double acc[3] = {0, 0, 0}, wsum = 0;
            for (int yy = static_cast<int>(a0); yy < sh && yy < a1; ++yy) {
                const double wy = std::min<double>(yy + 1, a1) - std::max<double>(yy, a0);
                if (wy <= 0) continue;
                for (int xx = static_cast<int>(b0); xx < sw && xx < b1; ++xx) {
                    const double wx = std::min<double>(xx + 1, b1) - std::max<double>(xx, b0);
                    if (wx <= 0) continue;
                    const uint8_t* px = src + (static_cast<size_t>(yy) * sw + xx) * 3; const double w = wx * wy;
                    acc[0] += w * px[0]; acc[1] += w * px[1]; acc[2] += w * px[2]; wsum += w;
                }
            }
            uint8_t* d = dst + static_cast<size_t>(y) * dstride + (static_cast<size_t>(dx0) + x) * 3;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(wsum > 0 ? std::lround(acc[c] / wsum) : 0);
        }
    }
}

bool display_letterbox_jpeg(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, int& src_w, int& src_h, std::string& err) {
    detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(in.data(), in.size(), img, err)) return false;
    src_w = img.width; src_h = img.height;
    std::vector<uint8_t> canvas(static_cast<size_t>(640) * 360 * 3, 0);                     // black
    // stretch to 1920x1080 then crop the top-left 1280x800: in source pixels the rectangle is [0, W*1280/1920) x [0, H*800/1080)
    area_resample_rgb(img.rgb.data(), img.width, img.height, 0, 0, img.width * (1280.0 / 1920.0), img.height * (800.0 / 1080.0),
                      canvas.data(), 576, 360, 640 * 3, 32);
    return detail::encode_rgb_jpeg(canvas.data(), 640, 360, 90, out, err);
}

bool PopStore::search_evidence(EmbeddingProvider& provider, const std::vector<uint8_t>& query_jpeg, const EvidenceSearchOptions& o,
                               EvidenceSearchResult& out, std::string& err) {
    out = EvidenceSearchResult{};
    if (o.top_k < 1 || o.top_k > 50) { err = "top_k must be 1..50"; return false; }
    if (o.has_min_similarity && !(o.min_similarity >= -1.f && o.min_similarity <= 1.f)) { err = "min_similarity must be in [-1,1]"; return false; }
    if (query_jpeg.empty() || query_jpeg.size() > kMaxJpegBytes) { err = "reference image missing or larger than 8 MiB"; return false; }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<float> q; int w = 0, h = 0;
    if (!decode_embed(provider, query_jpeg, q, w, h, err)) return false;          // same decoder + provider + normalisation as the evidence
    const auto t_embedded = std::chrono::steady_clock::now();
    out.embed_ms = std::chrono::duration<double, std::milli>(t_embedded - t0).count();
    const EmbeddingSpec spec = provider.spec();
    out.coverage = evidence_coverage(&spec, o.capture_method);
    struct Cand { int64_t id; float sim; int64_t utc; bool hist; };
    std::vector<Cand> top;                                                         // hits at or above the minimum, bounded to top_k
    Cand best{0, -2.f, 0, false};
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->db) { err = "store not open"; return false; }
        const int64_t mid = impl_->model_id(spec, false);
        const bool method_filter = !o.capture_method.empty();
        const size_t dim = q.size();
        auto worse = [](const Cand& a, const Cand& b) { return a.sim > b.sim || (a.sim == b.sim && a.utc > b.utc); };   // "a ranks before b"
        auto scan = [&](Stmt& st, bool hist) {
            while (!out.cancelled && sqlite3_step(st.s) == SQLITE_ROW) {
                if ((out.scanned & 255) == 0 && o.cancel && o.cancel()) { out.cancelled = true; break; }
                ++out.scanned;
                if (static_cast<size_t>(sqlite3_column_bytes(st.s, 1)) != dim * sizeof(float)) { ++out.skipped_invalid; continue; }
                const float* v = static_cast<const float*>(sqlite3_column_blob(st.s, 1));
                double dot = 0; for (size_t k = 0; k < dim; ++k) dot += static_cast<double>(v[k]) * q[k];
                if (!std::isfinite(dot)) { ++out.skipped_invalid; continue; }
                const Cand c{sqlite3_column_int64(st.s, 0), static_cast<float>(std::max(-1.0, std::min(1.0, dot))), sqlite3_column_int64(st.s, 2), hist};
                if (best.id == 0 || worse(c, best)) best = c;
                if (o.has_min_similarity && c.sim < o.min_similarity) continue;
                top.push_back(c);
                if (top.size() > static_cast<size_t>(o.top_k) * 4 + 64) { std::partial_sort(top.begin(), top.begin() + o.top_k, top.end(), worse); top.resize(o.top_k); }
            }
        };
        const bool want_checkpoints = o.role.empty() || o.role == "verification_checkpoint";
        const bool want_history = (o.role.empty() || o.role == "history_sample") && impl_->hist && (!method_filter || o.capture_method == "resilience_grpc");
        if (mid != 0 && want_checkpoints && !(method_filter && !impl_->v2 && o.capture_method != "brightscript_screenshot")) {
            std::string sql = "SELECT e.evidence_id,e.embedding,e.utc_ms FROM display_capture_evidence e";
            if (!o.media_id.empty()) sql += " JOIN playback_sessions s ON s.session_id=e.session_id";
            sql += " WHERE e.status='ok' AND e.embedded=1 AND e.model_id=?";
            if (method_filter && impl_->v2) sql += " AND e.capture_method=?";
            if (o.start_utc_ms > 0) sql += " AND e.utc_ms>=?";
            if (o.end_utc_ms > 0) sql += " AND e.utc_ms<?";
            if (o.require_image) sql += " AND e.image_available=1";
            if (!o.session_id.empty()) sql += " AND e.session_id=?";
            if (!o.media_id.empty()) sql += " AND (s.expected_media_id=? OR s.played_media_id=?)";
            Stmt st(impl_->db, sql + ";");
            if (!st) { err = sqlite3_errmsg(impl_->db); return false; }
            int i = 1;
            sqlite3_bind_int64(st.s, i++, mid);
            if (method_filter && impl_->v2) st.text(i++, o.capture_method);
            if (o.start_utc_ms > 0) sqlite3_bind_int64(st.s, i++, o.start_utc_ms);
            if (o.end_utc_ms > 0) sqlite3_bind_int64(st.s, i++, o.end_utc_ms);
            if (!o.session_id.empty()) st.text(i++, o.session_id);
            if (!o.media_id.empty()) { st.text(i++, o.media_id); st.text(i++, o.media_id); }
            scan(st, false);
        }
        if (mid != 0 && want_history && !out.cancelled) {
            // A history row that reuses a verification checkpoint (evidence_id set, or a checkpoint of the same session with the same capture time)
            // is not scanned again: the checkpoint row represents that frame, so nothing is counted twice.
            std::string sql = "SELECT h.frame_id,h.embedding,h.utc_ms FROM display_history_frames h";
            if (!o.media_id.empty()) sql += " JOIN playback_sessions s ON s.session_id=h.session_id";
            sql += " WHERE h.status='ok' AND h.embedded=1 AND h.evidence_id IS NULL AND h.model_id=?"
                   " AND NOT EXISTS (SELECT 1 FROM display_capture_evidence e WHERE e.session_id=h.session_id AND e.utc_ms=h.utc_ms AND e.status='ok' AND e.embedded=1 AND e.capture_method='resilience_grpc')";
            if (o.start_utc_ms > 0) sql += " AND h.utc_ms>=?";
            if (o.end_utc_ms > 0) sql += " AND h.utc_ms<?";
            if (o.require_image) sql += " AND h.image_available=1";
            if (!o.session_id.empty()) sql += " AND h.session_id=?";
            if (!o.media_id.empty()) sql += " AND (s.expected_media_id=? OR s.played_media_id=?)";
            Stmt st(impl_->db, sql + ";");
            if (!st) { err = sqlite3_errmsg(impl_->db); return false; }
            int i = 1;
            sqlite3_bind_int64(st.s, i++, mid);
            if (o.start_utc_ms > 0) sqlite3_bind_int64(st.s, i++, o.start_utc_ms);
            if (o.end_utc_ms > 0) sqlite3_bind_int64(st.s, i++, o.end_utc_ms);
            if (!o.session_id.empty()) st.text(i++, o.session_id);
            if (!o.media_id.empty()) { st.text(i++, o.media_id); st.text(i++, o.media_id); }
            scan(st, true);
        }
        auto worse2 = worse;
        std::sort(top.begin(), top.end(), worse2);
        if (top.size() > static_cast<size_t>(o.top_k)) top.resize(o.top_k);
    }
    auto make_hit = [&](const Cand& c, EvidenceHit& hit) {
        hit.similarity = c.sim;
        if (!c.hist) return get_evidence(c.id, hit.evidence);
        HistoryFrame hf;
        if (!get_history(c.id, hf)) return false;
        Evidence& e = hit.evidence; e = Evidence{};
        e.role = "history_sample"; e.history_id = hf.frame_id; e.sec_idx = hf.sec_idx; e.session_id = hf.session_id; e.idx = hf.sec_idx;
        e.target_offset_ms = hf.target_offset_ms; e.actual_offset_ms = hf.actual_offset_ms; e.status = hf.status; e.source = hf.source; e.path = hf.path;
        e.sha256 = hf.sha256; e.width = hf.width; e.height = hf.height; e.bytes = hf.bytes; e.utc_ms = hf.utc_ms; e.embedded = hf.embedded;
        e.image_available = hf.image_available; e.capture_method = hf.capture_method; e.provenance = hf.provenance; e.uncertainty_ms = hf.uncertainty_ms;
        e.expected_media_id = hf.expected_media_id;
        return true;
    };
    for (const auto& c : top) { EvidenceHit hit; if (make_hit(c, hit)) out.hits.push_back(std::move(hit)); }
    if (best.id != 0) { EvidenceHit hit; if (make_hit(best, hit)) { out.closest = std::move(hit); out.has_closest = true; } }
    out.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    out.scan_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_embedded).count();
    return true;
}

// ---- retention --------------------------------------------------------------------------------------------------
uint64_t PopStore::prune_evidence(int64_t older_than_ms, uint64_t max_bytes) {
    std::vector<std::pair<int64_t, std::pair<std::string, uint64_t>>> rows;   // evidence_id -> (path, bytes), oldest first
    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->db) return 0;
        Stmt q(impl_->db, "SELECT evidence_id,path,bytes,utc_ms FROM display_capture_evidence WHERE image_available=1 ORDER BY utc_ms,evidence_id;");
        std::vector<int64_t> times;
        while (q && sqlite3_step(q.s) == SQLITE_ROW) {
            rows.push_back({sqlite3_column_int64(q.s, 0), {col_text(q.s, 1), static_cast<uint64_t>(sqlite3_column_int64(q.s, 2))}});
            times.push_back(sqlite3_column_int64(q.s, 3));
            total += rows.back().second.second;
        }
        uint64_t removed = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            const bool old = older_than_ms > 0 && times[i] < older_than_ms;
            const bool over = max_bytes > 0 && total > max_bytes;
            if (!old && !over) break;
            std::error_code ec;
            fs::remove(fs::path(impl_->cfg.root_directory) / rows[i].second.first, ec);
            Stmt u(impl_->db, "UPDATE display_capture_evidence SET image_available=0 WHERE evidence_id=?;");
            if (u) { sqlite3_bind_int64(u.s, 1, rows[i].first); sqlite3_step(u.s); }
            total -= rows[i].second.second; ++removed;
        }
        return removed;
    }
}

} // namespace visualmemory
