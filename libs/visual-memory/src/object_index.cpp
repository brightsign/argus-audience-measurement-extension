// SPDX-License-Identifier: MIT
#include "visualmemory/object_index.h"

#include "embedding_math.h"
#include "jpeg_decoder.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <queue>

namespace visualmemory {

namespace {
constexpr int kSchemaVersion = 3;
constexpr const char* kCropDir = "object_crops";

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
double since_ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
std::string col_text(sqlite3_stmt* s, int c) {
    const unsigned char* t = sqlite3_column_text(s, c);
    return t ? reinterpret_cast<const char*>(t) : std::string{};
}
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) s = nullptr;
    }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
};
bool same_space(const EmbeddingSpec& a, const EmbeddingSpec& b) {
    return a.model_name == b.model_name && a.model_version == b.model_version && a.model_sha256 == b.model_sha256 &&
           a.preprocessing == b.preprocessing && a.dimension == b.dimension && a.dtype == b.dtype && a.metric == b.metric;
}
SearchStatus mk(SearchStatus::Code c, std::string m) { SearchStatus s; s.code = c; s.message = std::move(m); return s; }

// Dot product with 4 independent accumulators (no fast-math needed): lets the compiler use SIMD / ILP on the RK3588.
float dot_product(const float* a, const float* b, int n) {
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0; int i = 0;
    for (; i + 4 <= n; i += 4) { s0 += a[i] * b[i]; s1 += a[i + 1] * b[i + 1]; s2 += a[i + 2] * b[i + 2]; s3 += a[i + 3] * b[i + 3]; }
    for (; i < n; ++i) s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

const char* kRegionCols =
    "region_id,frame_id,source_id,capture_utc_ms,bbox_x,bbox_y,bbox_width,bbox_height,image_width,image_height,"
    "object_label,model_id,embedding_status,crop_path,created_at,frame_sha256,"
    "index_origin,class_id,class_name,confidence,detector,detector_version,crop_bytes,crop_width,crop_height,crop_removed_at,"
    "source_type,source_kind,pop_session_id,expected_media_id,reported_media_id,playback_offset_ms,time_uncertainty_ms,provenance";

ObjectRegion row_region(sqlite3_stmt* s) {
    ObjectRegion r;
    r.region_id = sqlite3_column_int64(s, 0); r.frame_id = sqlite3_column_int64(s, 1);
    r.source_id = col_text(s, 2); r.capture_utc_ms = sqlite3_column_int64(s, 3);
    r.roi = {sqlite3_column_int(s, 4), sqlite3_column_int(s, 5), sqlite3_column_int(s, 6), sqlite3_column_int(s, 7)};
    r.image_width = sqlite3_column_int(s, 8); r.image_height = sqlite3_column_int(s, 9);
    r.label = col_text(s, 10); r.model_id = sqlite3_column_int64(s, 11);
    r.embedding_status = sqlite3_column_int(s, 12);
    r.created_utc_ms = sqlite3_column_int64(s, 14); r.frame_sha256 = col_text(s, 15);
    r.index_origin = col_text(s, 16); if (r.index_origin.empty()) r.index_origin = "manual";
    r.class_id = sqlite3_column_type(s, 17) == SQLITE_NULL ? -1 : sqlite3_column_int(s, 17);
    r.class_name = col_text(s, 18);
    r.confidence = sqlite3_column_type(s, 19) == SQLITE_NULL ? -1.0f : static_cast<float>(sqlite3_column_double(s, 19));
    r.detector = col_text(s, 20); r.detector_version = col_text(s, 21);
    r.crop_bytes = static_cast<uint64_t>(sqlite3_column_int64(s, 22));
    r.crop_width = sqlite3_column_int(s, 23); r.crop_height = sqlite3_column_int(s, 24);
    const bool has_path = sqlite3_column_type(s, 13) != SQLITE_NULL;
    const bool removed = sqlite3_column_type(s, 25) != SQLITE_NULL;
    r.crop_state = !has_path ? "none" : (removed ? "expired" : "retained");
    r.has_crop_file = r.crop_state == "retained";
    if (has_path) r.crop_rel = col_text(s, 13);
    r.source_type = col_text(s, 26); if (r.source_type.empty()) r.source_type = "camera";
    r.source_kind = col_text(s, 27); r.pop_session_id = col_text(s, 28); r.expected_media_id = col_text(s, 29); r.reported_media_id = col_text(s, 30);
    r.playback_offset_ms = sqlite3_column_type(s, 31) == SQLITE_NULL ? -1 : sqlite3_column_int64(s, 31);
    r.time_uncertainty_ms = sqlite3_column_type(s, 32) == SQLITE_NULL ? -1 : sqlite3_column_int(s, 32);
    r.provenance = col_text(s, 33);
    return r;
}

// Crop [roi] out of packed RGB, optionally scaled so the longest side <= max_side (bilinear, aspect preserved).
void crop_rgb(const detail::DecodedImage& img, const Roi& roi, int max_side, std::vector<uint8_t>& out, int& ow, int& oh) {
    ow = roi.w; oh = roi.h;
    if (max_side > 0 && std::max(roi.w, roi.h) > max_side) {
        const double s = static_cast<double>(max_side) / std::max(roi.w, roi.h);
        ow = std::max(1, static_cast<int>(std::lround(roi.w * s))); oh = std::max(1, static_cast<int>(std::lround(roi.h * s)));
    }
    out.assign(static_cast<size_t>(ow) * oh * 3, 0);
    for (int y = 0; y < oh; ++y) {
        const double fy = (y + 0.5) * roi.h / oh - 0.5;
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, roi.h - 1), y1 = std::min(y0 + 1, roi.h - 1);
        const double wy = std::clamp(fy - y0, 0.0, 1.0);
        for (int x = 0; x < ow; ++x) {
            const double fx = (x + 0.5) * roi.w / ow - 0.5;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, roi.w - 1), x1 = std::min(x0 + 1, roi.w - 1);
            const double wx = std::clamp(fx - x0, 0.0, 1.0);
            for (int c = 0; c < 3; ++c) {
                auto px = [&](int xx, int yy) { return img.rgb[(static_cast<size_t>(roi.y + yy) * img.width + roi.x + xx) * 3 + c]; };
                const double v = (px(x0, y0) * (1 - wx) + px(x1, y0) * wx) * (1 - wy) + (px(x0, y1) * (1 - wx) + px(x1, y1) * wx) * wy;
                out[(static_cast<size_t>(y) * ow + x) * 3 + c] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
            }
        }
    }
}
// Source columns of an object row (see schema v3): where the parent frame came from and, for display frames, its Proof-of-Play facts.
void bind_source(sqlite3_stmt* st, int i, const ObjSrcFrame& f) {
    sqlite3_bind_text(st, i, f.source_type.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, i + 1, f.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, i + 2, f.session_id.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, i + 3, f.expected_media_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, i + 4, f.reported_media_id.c_str(), -1, SQLITE_TRANSIENT);
    if (f.source_type == "display" && f.playback_offset_ms >= 0) sqlite3_bind_int64(st, i + 5, f.playback_offset_ms); else sqlite3_bind_null(st, i + 5);
    if (f.source_type == "display" && f.time_uncertainty_ms >= 0) sqlite3_bind_int(st, i + 6, f.time_uncertainty_ms); else sqlite3_bind_null(st, i + 6);
    const std::string prov = f.provenance.empty() ? f.capture_method : f.capture_method.empty() ? f.provenance : f.capture_method + " / " + f.provenance;
    sqlite3_bind_text(st, i + 7, prov.c_str(), -1, SQLITE_TRANSIENT);
}
} // namespace

const char* to_string(RoiCheck c) {
    switch (c) {
        case RoiCheck::Ok: return "ok";
        case RoiCheck::Invalid: return "invalid rectangle";
        case RoiCheck::TooSmall: return "selection too small";
        case RoiCheck::OutOfBounds: return "selection outside the image";
    }
    return "?";
}

RoiCheck validate_roi(const Roi& r, int iw, int ih, int min_px) {
    if (iw < 1 || ih < 1 || r.w < 1 || r.h < 1 || r.x < 0 || r.y < 0) return RoiCheck::Invalid;
    if (static_cast<int64_t>(r.x) + r.w > iw || static_cast<int64_t>(r.y) + r.h > ih) return RoiCheck::OutOfBounds;
    if (r.w < min_px || r.h < min_px) return RoiCheck::TooSmall;
    return RoiCheck::Ok;
}

struct ObjectIndex::Impl {
    ObjectIndexConfig cfg;
    std::mutex mu;
    sqlite3* db = nullptr;
    std::atomic<sqlite3*> intr{nullptr};

    explicit Impl(ObjectIndexConfig c) : cfg(std::move(c)) {}

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
        if (v > kSchemaVersion) { err = "objects.db schema " + std::to_string(v) + " is newer than this library (" + std::to_string(kSchemaVersion) + ")"; return false; }
        if (v < 1) {
            if (!exec("BEGIN IMMEDIATE;", err)) return false;
            const char* ddl =
                "CREATE TABLE IF NOT EXISTS object_models(model_id INTEGER PRIMARY KEY,model_name TEXT NOT NULL,model_version TEXT NOT NULL,"
                " model_sha256 TEXT NOT NULL,preprocessing TEXT NOT NULL,dimension INTEGER NOT NULL,dtype TEXT NOT NULL,metric TEXT NOT NULL,"
                " UNIQUE(model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric));"
                "CREATE TABLE IF NOT EXISTS object_regions(region_id INTEGER PRIMARY KEY AUTOINCREMENT,frame_id INTEGER NOT NULL,"
                " source_id TEXT NOT NULL,capture_utc_ms INTEGER NOT NULL,bbox_x INTEGER NOT NULL,bbox_y INTEGER NOT NULL,"
                " bbox_width INTEGER NOT NULL,bbox_height INTEGER NOT NULL,image_width INTEGER NOT NULL,image_height INTEGER NOT NULL,"
                " object_label TEXT NOT NULL DEFAULT '',model_id INTEGER NOT NULL REFERENCES object_models(model_id),"
                " embedding_status INTEGER NOT NULL DEFAULT 1,crop_path TEXT,created_at INTEGER NOT NULL,frame_sha256 TEXT NOT NULL DEFAULT '',"
                " UNIQUE(frame_id,bbox_x,bbox_y,bbox_width,bbox_height,model_id));"
                "CREATE TABLE IF NOT EXISTS object_vectors(region_id INTEGER PRIMARY KEY REFERENCES object_regions(region_id) ON DELETE CASCADE,"
                " embedding BLOB NOT NULL);"
                "CREATE INDEX IF NOT EXISTS ix_obj_time ON object_regions(capture_utc_ms);"
                "CREATE INDEX IF NOT EXISTS ix_obj_src ON object_regions(source_id,capture_utc_ms);"
                "CREATE INDEX IF NOT EXISTS ix_obj_label ON object_regions(object_label);"
                "CREATE INDEX IF NOT EXISTS ix_obj_frame ON object_regions(frame_id);"
                "PRAGMA user_version=1;";
            if (!exec(ddl, err)) { std::string e2; exec("ROLLBACK;", e2); return false; }
            if (!exec("COMMIT;", err)) return false;
        }
        if (user_version() < 2 && !migrate_v2(err)) return false;
        if (user_version() < 3) return migrate_v3(err);
        return true;
    }
    // v2 -> v3: source-aware identity. SQLite cannot alter a UNIQUE constraint, so object_regions and object_frame_progress are rebuilt
    // with the source in their key. Every row, region_id and the AUTOINCREMENT counter are preserved, vectors are untouched (foreign
    // keys are switched off for the rebuild: DROP TABLE on the parent would otherwise cascade-delete object_vectors).
    // A verbatim copy of the v2 file is kept as objects.db.pre-v3 first. Fails (changing nothing) rather than losing data.
    bool migrate_v3(std::string& err) {
        const std::filesystem::path src = std::filesystem::path(cfg.root_directory) / "objects.db";
        const std::filesystem::path bak = std::filesystem::path(cfg.root_directory) / "objects.db.pre-v3";
        std::error_code ec;
        if (count_all() > 0 && !std::filesystem::exists(bak, ec)) {
            exec("PRAGMA wal_checkpoint(TRUNCATE);", err);
            std::filesystem::copy_file(src, bak, std::filesystem::copy_options::none, ec);
            if (ec) { err = "cannot back up objects.db before the v3 migration: " + ec.message(); return false; }
        }
        auto fk_violations = [&]() { uint64_t n = 0; Stmt q(db, "PRAGMA foreign_key_check;"); if (q) while (sqlite3_step(q.s) == SQLITE_ROW) ++n; return n; };
        const uint64_t fk_before = fk_violations();                  // pre-existing orphans (if any) are carried over, not turned into a failure
        uint64_t before = count_all(), vec_before = 0;
        { Stmt q(db, "SELECT COUNT(*) FROM object_vectors;"); if (q && sqlite3_step(q.s) == SQLITE_ROW) vec_before = static_cast<uint64_t>(sqlite3_column_int64(q.s, 0)); }
        int64_t seq = 0;
        { Stmt q(db, "SELECT seq FROM sqlite_sequence WHERE name='object_regions';"); if (q && sqlite3_step(q.s) == SQLITE_ROW) seq = sqlite3_column_int64(q.s, 0); }
        if (!exec("PRAGMA foreign_keys=OFF;", err)) return false;
        auto fail = [&](const std::string& why) { std::string e2; exec("ROLLBACK;", e2); exec("PRAGMA foreign_keys=ON;", e2); err = why; return false; };
        if (!exec("BEGIN IMMEDIATE;", err)) { std::string e2; exec("PRAGMA foreign_keys=ON;", e2); return false; }
        const char* ddl =
            "CREATE TABLE object_regions_v3(region_id INTEGER PRIMARY KEY AUTOINCREMENT,frame_id INTEGER NOT NULL,"
            " source_id TEXT NOT NULL,capture_utc_ms INTEGER NOT NULL,bbox_x INTEGER NOT NULL,bbox_y INTEGER NOT NULL,"
            " bbox_width INTEGER NOT NULL,bbox_height INTEGER NOT NULL,image_width INTEGER NOT NULL,image_height INTEGER NOT NULL,"
            " object_label TEXT NOT NULL DEFAULT '',model_id INTEGER NOT NULL REFERENCES object_models(model_id),"
            " embedding_status INTEGER NOT NULL DEFAULT 1,crop_path TEXT,created_at INTEGER NOT NULL,frame_sha256 TEXT NOT NULL DEFAULT '',"
            " index_origin TEXT NOT NULL DEFAULT 'manual',class_id INTEGER,class_name TEXT NOT NULL DEFAULT '',confidence REAL,"
            " detector TEXT NOT NULL DEFAULT '',detector_version TEXT NOT NULL DEFAULT '',crop_bytes INTEGER NOT NULL DEFAULT 0,"
            " crop_width INTEGER NOT NULL DEFAULT 0,crop_height INTEGER NOT NULL DEFAULT 0,crop_removed_at INTEGER,"
            " source_type TEXT NOT NULL DEFAULT 'camera',source_kind TEXT NOT NULL DEFAULT '',pop_session_id TEXT NOT NULL DEFAULT '',"
            " expected_media_id TEXT NOT NULL DEFAULT '',reported_media_id TEXT NOT NULL DEFAULT '',playback_offset_ms INTEGER,"
            " time_uncertainty_ms INTEGER,provenance TEXT NOT NULL DEFAULT '',"
            " UNIQUE(source_type,source_kind,frame_id,bbox_x,bbox_y,bbox_width,bbox_height,model_id));"
            "INSERT INTO object_regions_v3(region_id,frame_id,source_id,capture_utc_ms,bbox_x,bbox_y,bbox_width,bbox_height,image_width,image_height,"
            " object_label,model_id,embedding_status,crop_path,created_at,frame_sha256,index_origin,class_id,class_name,confidence,detector,"
            " detector_version,crop_bytes,crop_width,crop_height,crop_removed_at)"
            " SELECT region_id,frame_id,source_id,capture_utc_ms,bbox_x,bbox_y,bbox_width,bbox_height,image_width,image_height,"
            " object_label,model_id,embedding_status,crop_path,created_at,frame_sha256,index_origin,class_id,class_name,confidence,detector,"
            " detector_version,crop_bytes,crop_width,crop_height,crop_removed_at FROM object_regions;"
            "DROP TABLE object_regions;"
            "ALTER TABLE object_regions_v3 RENAME TO object_regions;"
            "CREATE INDEX ix_obj_time ON object_regions(capture_utc_ms);"
            "CREATE INDEX ix_obj_src ON object_regions(source_id,capture_utc_ms);"
            "CREATE INDEX ix_obj_label ON object_regions(object_label);"
            "CREATE INDEX ix_obj_frame ON object_regions(source_type,source_kind,frame_id);"
            "CREATE INDEX ix_obj_origin ON object_regions(index_origin,class_name);"
            "CREATE INDEX ix_obj_source ON object_regions(source_type,class_name,capture_utc_ms);"
            "CREATE INDEX ix_obj_session ON object_regions(pop_session_id) WHERE pop_session_id<>'';"
            "CREATE TABLE object_frame_progress_v3(source_type TEXT NOT NULL DEFAULT 'camera',source_kind TEXT NOT NULL DEFAULT '',"
            " frame_id INTEGER NOT NULL,detector TEXT NOT NULL,detector_version TEXT NOT NULL,source_id TEXT NOT NULL DEFAULT '',"
            " capture_utc_ms INTEGER NOT NULL,status TEXT NOT NULL,objects_found INTEGER NOT NULL DEFAULT 0,objects_indexed INTEGER NOT NULL DEFAULT 0,"
            " threshold REAL,error TEXT NOT NULL DEFAULT '',processed_at INTEGER NOT NULL,"
            " PRIMARY KEY(source_type,source_kind,frame_id,detector,detector_version)) WITHOUT ROWID;"
            "INSERT INTO object_frame_progress_v3(frame_id,detector,detector_version,source_id,capture_utc_ms,status,objects_found,objects_indexed,threshold,error,processed_at)"
            " SELECT frame_id,detector,detector_version,source_id,capture_utc_ms,status,objects_found,objects_indexed,threshold,error,processed_at FROM object_frame_progress;"
            "DROP TABLE object_frame_progress;"
            "ALTER TABLE object_frame_progress_v3 RENAME TO object_frame_progress;"
            "CREATE INDEX ix_obj_prog_time ON object_frame_progress(source_type,capture_utc_ms);";
        if (!exec(ddl, err)) return fail(err);
        // keep the AUTOINCREMENT counter at least where it was (ids of deleted regions are never reused). sqlite_sequence has no UNIQUE key on
        // name, so UPDATE the renamed table's row (INSERT OR REPLACE would add a second row).
        {
            Stmt u(db, "UPDATE sqlite_sequence SET seq=MAX(?1,seq) WHERE name='object_regions';");
            if (u) { sqlite3_bind_int64(u.s, 1, seq); sqlite3_step(u.s); }
            if (sqlite3_changes(db) == 0 && seq > 0) {
                Stmt i(db, "INSERT INTO sqlite_sequence(name,seq) VALUES('object_regions',?1);");
                if (i) { sqlite3_bind_int64(i.s, 1, seq); sqlite3_step(i.s); }
            }
        }
        if (count_all() != before) return fail("v3 migration would change the number of regions: aborted, nothing changed");
        { Stmt q(db, "SELECT COUNT(*) FROM object_vectors;"); if (q && sqlite3_step(q.s) == SQLITE_ROW && static_cast<uint64_t>(sqlite3_column_int64(q.s, 0)) != vec_before) return fail("v3 migration would change the vectors: aborted, nothing changed"); }
        if (fk_violations() > fk_before) return fail("v3 migration: foreign key check found new violations, aborted, nothing changed");
        if (!exec("PRAGMA user_version=3;", err)) return fail(err);
        if (!exec("COMMIT;", err)) return fail(err);
        std::string e2; exec("PRAGMA foreign_keys=ON;", e2);
        return true;
    }
    // v1 -> v2 (additive): new nullable/defaulted columns on object_regions, a progress table and a meta table.
    // A copy of the v1 file is kept next to it (objects.db.pre-v2) before the first change; nothing is deleted.
    bool migrate_v2(std::string& err) {
        const std::filesystem::path src = std::filesystem::path(cfg.root_directory) / "objects.db";
        const std::filesystem::path bak = std::filesystem::path(cfg.root_directory) / "objects.db.pre-v2";
        std::error_code ec;
        if (user_version() == 1 && count_all() > 0 && !std::filesystem::exists(bak, ec)) {
            exec("PRAGMA wal_checkpoint(TRUNCATE);", err);
            std::filesystem::copy_file(src, bak, std::filesystem::copy_options::none, ec);
            if (ec) { err = "cannot back up objects.db before the v2 migration: " + ec.message(); return false; }
        }
        if (!exec("BEGIN IMMEDIATE;", err)) return false;
        const char* ddl =
            "ALTER TABLE object_regions ADD COLUMN index_origin TEXT NOT NULL DEFAULT 'manual';"
            "ALTER TABLE object_regions ADD COLUMN class_id INTEGER;"
            "ALTER TABLE object_regions ADD COLUMN class_name TEXT NOT NULL DEFAULT '';"
            "ALTER TABLE object_regions ADD COLUMN confidence REAL;"
            "ALTER TABLE object_regions ADD COLUMN detector TEXT NOT NULL DEFAULT '';"
            "ALTER TABLE object_regions ADD COLUMN detector_version TEXT NOT NULL DEFAULT '';"
            "ALTER TABLE object_regions ADD COLUMN crop_bytes INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE object_regions ADD COLUMN crop_width INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE object_regions ADD COLUMN crop_height INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE object_regions ADD COLUMN crop_removed_at INTEGER;"
            "CREATE INDEX IF NOT EXISTS ix_obj_origin ON object_regions(index_origin,class_name);"
            "CREATE TABLE IF NOT EXISTS object_frame_progress(frame_id INTEGER NOT NULL,detector TEXT NOT NULL,detector_version TEXT NOT NULL,"
            " source_id TEXT NOT NULL DEFAULT '',capture_utc_ms INTEGER NOT NULL,status TEXT NOT NULL,objects_found INTEGER NOT NULL DEFAULT 0,"
            " objects_indexed INTEGER NOT NULL DEFAULT 0,threshold REAL,error TEXT NOT NULL DEFAULT '',processed_at INTEGER NOT NULL,"
            " PRIMARY KEY(frame_id,detector,detector_version)) WITHOUT ROWID;"
            "CREATE INDEX IF NOT EXISTS ix_obj_prog_time ON object_frame_progress(capture_utc_ms);"
            "CREATE TABLE IF NOT EXISTS object_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
            "PRAGMA user_version=2;";
        if (!exec(ddl, err)) { std::string e2; exec("ROLLBACK;", e2); return false; }
        return exec("COMMIT;", err);
    }
    std::filesystem::path crop_abs(const std::string& rel) const { return std::filesystem::path(cfg.root_directory) / rel; }
    static void remove_file(const std::filesystem::path& p) { std::error_code ec; std::filesystem::remove(p, ec); }
    // Atomic write: temp file + rename. Returns false on any failure (the target is then absent).
    bool write_crop_file(const std::string& rel, const std::vector<uint8_t>& jpeg) {
        std::error_code ec;
        const auto dst = crop_abs(rel);
        std::filesystem::create_directories(dst.parent_path(), ec);
        const auto tmp = std::filesystem::path(dst.string() + ".tmp");
        FILE* f = std::fopen(tmp.string().c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(jpeg.data(), 1, jpeg.size(), f) == jpeg.size();
        const bool cl = std::fclose(f) == 0;
        if (!ok || !cl) { remove_file(tmp); return false; }
        std::filesystem::rename(tmp, dst, ec);
        if (ec) { remove_file(tmp); return false; }
        return true;
    }
    int64_t model_id(const EmbeddingSpec& s, bool create) {
        {
            Stmt q(db, "SELECT model_id FROM object_models WHERE model_name=? AND model_version=? AND model_sha256=? AND "
                       "preprocessing=? AND dimension=? AND dtype=? AND metric=?;");
            if (!q) return 0;
            auto bt = [&](int i, const std::string& v) { sqlite3_bind_text(q.s, i, v.c_str(), -1, SQLITE_TRANSIENT); };
            bt(1, s.model_name); bt(2, s.model_version); bt(3, s.model_sha256); bt(4, s.preprocessing);
            sqlite3_bind_int(q.s, 5, s.dimension); bt(6, s.dtype); bt(7, s.metric);
            if (sqlite3_step(q.s) == SQLITE_ROW) return sqlite3_column_int64(q.s, 0);
        }
        if (!create) return 0;
        Stmt ins(db, "INSERT INTO object_models(model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric) VALUES(?,?,?,?,?,?,?);");
        if (!ins) return 0;
        auto bt = [&](int i, const std::string& v) { sqlite3_bind_text(ins.s, i, v.c_str(), -1, SQLITE_TRANSIENT); };
        bt(1, s.model_name); bt(2, s.model_version); bt(3, s.model_sha256); bt(4, s.preprocessing);
        sqlite3_bind_int(ins.s, 5, s.dimension); bt(6, s.dtype); bt(7, s.metric);
        return sqlite3_step(ins.s) == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : 0;
    }
    bool model_spec(int64_t id, EmbeddingSpec& out) {
        Stmt q(db, "SELECT model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric FROM object_models WHERE model_id=?;");
        if (!q) return false;
        sqlite3_bind_int64(q.s, 1, id);
        if (sqlite3_step(q.s) != SQLITE_ROW) return false;
        out.model_name = col_text(q.s, 0); out.model_version = col_text(q.s, 1); out.model_sha256 = col_text(q.s, 2);
        out.preprocessing = col_text(q.s, 3); out.dimension = sqlite3_column_int(q.s, 4); out.dtype = col_text(q.s, 5);
        out.metric = col_text(q.s, 6);
        return true;
    }
    std::filesystem::path crop_abs_of(const ObjectRegion& r) const {
        return crop_abs(r.crop_rel.empty() ? std::string(kCropDir) + "/" + std::to_string(r.region_id) + ".jpg" : r.crop_rel);
    }
    uint64_t count_all() {
        Stmt q(db, "SELECT COUNT(*) FROM object_regions;");
        return (q && sqlite3_step(q.s) == SQLITE_ROW) ? static_cast<uint64_t>(sqlite3_column_int64(q.s, 0)) : 0;
    }

    ObjectSearchResult scan(const ObjectSources& avail, const std::vector<float>& qin, const EmbeddingSpec& space,
                            const ObjectSearchOptions& o) {
        ObjectSearchResult res; res.space = space;
        const auto t0 = std::chrono::steady_clock::now();
        auto fin = [&](SearchStatus::Code c, const std::string& m) { res.status = mk(c, m); res.status.elapsed_ms = since_ms(t0); return res; };
        if (!db) return fin(SearchStatus::Code::NotEnabled, "object index not open");
        if (o.top_k < 1) return fin(SearchStatus::Code::InvalidArgument, "top_k must be >= 1");
        if (o.has_min_similarity && !std::isfinite(o.min_similarity)) return fin(SearchStatus::Code::InvalidArgument, "min_similarity not finite");
        std::vector<float> q = qin;
        const detail::VecCheck vc = detail::l2_normalize(q, space.dimension);
        if (vc == detail::VecCheck::BadDimension) return fin(SearchStatus::Code::DimensionMismatch, "query dimension " + std::to_string(qin.size()) + " != " + std::to_string(space.dimension));
        if (vc != detail::VecCheck::Ok) return fin(SearchStatus::Code::InvalidVector, std::string("invalid query vector: ") + detail::to_string(vc));
        const int64_t mid = model_id(space, false);
        if (mid == 0) {
            const uint64_t any = count_all();
            if (any == 0) return fin(SearchStatus::Code::Ok, "object index is empty");
            return fin(SearchStatus::Code::ModelMismatch,
                       "the object index holds regions embedded with a different model/preprocessing (" + space.model_name + "/" + space.model_version + "/" + space.preprocessing + " not found)");
        }
        std::string where = " WHERE r.model_id=? AND r.embedding_status=1";
        if (o.start_utc_ms > 0) where += " AND r.capture_utc_ms>=?";
        if (o.end_utc_ms > 0) where += " AND r.capture_utc_ms<?";
        if (!o.source_id.empty()) where += " AND r.source_id=?";
        if (!o.label.empty()) where += " AND r.object_label=?";
        if (!o.source_type.empty()) where += " AND r.source_type=?";
        if (!o.index_origin.empty()) where += " AND r.index_origin=?";
        if (!o.class_name.empty()) where += " AND r.class_name=?";
        if (o.exclude_region_id > 0) where += " AND r.region_id<>?";
        Stmt st(db, "SELECT r.region_id,v.embedding FROM object_regions r JOIN object_vectors v ON v.region_id=r.region_id" + where + ";");
        if (!st) return fin(SearchStatus::Code::DatabaseError, sqlite3_errmsg(db));
        int bi = 1; sqlite3_bind_int64(st.s, bi++, mid);
        if (o.start_utc_ms > 0) sqlite3_bind_int64(st.s, bi++, o.start_utc_ms);
        if (o.end_utc_ms > 0) sqlite3_bind_int64(st.s, bi++, o.end_utc_ms);
        if (!o.source_id.empty()) sqlite3_bind_text(st.s, bi++, o.source_id.c_str(), -1, SQLITE_TRANSIENT);
        if (!o.label.empty()) sqlite3_bind_text(st.s, bi++, o.label.c_str(), -1, SQLITE_TRANSIENT);
        if (!o.source_type.empty()) sqlite3_bind_text(st.s, bi++, o.source_type.c_str(), -1, SQLITE_TRANSIENT);
        if (!o.index_origin.empty()) sqlite3_bind_text(st.s, bi++, o.index_origin.c_str(), -1, SQLITE_TRANSIENT);
        if (!o.class_name.empty()) sqlite3_bind_text(st.s, bi++, o.class_name.c_str(), -1, SQLITE_TRANSIENT);
        if (o.exclude_region_id > 0) sqlite3_bind_int64(st.s, bi++, o.exclude_region_id);

        const int fetch = std::min(std::max(o.top_k, 1), 100) * (o.require_image ? 4 : 1);
        using Cand = std::pair<float, int64_t>;                      // (similarity, region_id)
        auto worse = [](const Cand& a, const Cand& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; };
        std::priority_queue<Cand, std::vector<Cand>, decltype(worse)> heap(worse);   // min-heap on similarity
        const size_t bytes = static_cast<size_t>(space.dimension) * sizeof(float);
        uint64_t scanned = 0; int rc;
        intr = db;
        while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) {
            if (sqlite3_column_bytes(st.s, 1) != static_cast<int>(bytes)) continue;   // corrupt row
            const float* v = static_cast<const float*>(sqlite3_column_blob(st.s, 1));
            const float dot = dot_product(q.data(), v, space.dimension);
            ++scanned;
            if (o.has_min_similarity && dot < o.min_similarity) continue;
            const Cand c{dot, sqlite3_column_int64(st.s, 0)};
            if (static_cast<int>(heap.size()) < fetch) heap.push(c);
            else if (worse(c, heap.top())) { heap.pop(); heap.push(c); }   // c is better than the current worst
        }
        intr = nullptr;
        res.index_size = scanned;
        if (rc == SQLITE_INTERRUPT) { res.status = mk(SearchStatus::Code::Cancelled, "search interrupted"); res.status.elapsed_ms = since_ms(t0); return res; }
        if (rc != SQLITE_DONE) return fin(SearchStatus::Code::DatabaseError, sqlite3_errmsg(db));
        std::vector<Cand> top;
        while (!heap.empty()) { top.push_back(heap.top()); heap.pop(); }
        std::sort(top.begin(), top.end(), [](const Cand& a, const Cand& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        for (const auto& c : top) {
            Stmt rq(db, std::string("SELECT ") + kRegionCols + " FROM object_regions WHERE region_id=?;");
            if (!rq) continue;
            sqlite3_bind_int64(rq.s, 1, c.second);
            if (sqlite3_step(rq.s) != SQLITE_ROW) continue;
            ObjectHit h; h.region = row_region(rq.s); h.similarity = c.first; h.distance = 1.0f - c.first;
            if (ObjectFrameSource* fs = avail.get(h.region.source_type)) {     // the parent frame is resolved in ITS OWN source
                std::string oe; ObjSrcFrame sf;
                if (fs->ensure_open(oe) && fs->get(h.region.source_kind, h.region.frame_id, sf)) { h.image_available = sf.image_available; h.image_exists = sf.image_exists; }
            }
            if (h.region.crop_state == "retained") {      // the crop file itself must still be there
                std::error_code ec; h.crop_available = std::filesystem::exists(crop_abs_of(h.region), ec);
            }
            if (o.require_image && !h.image_exists && !h.crop_available) continue;
            res.hits.push_back(std::move(h));
            if (static_cast<int>(res.hits.size()) >= std::min(o.top_k, 100)) break;
        }
        return fin(SearchStatus::Code::Ok, res.hits.empty() && scanned == 0 ? "no indexed objects match the filters" : "");
    }
};

ObjectIndex::ObjectIndex(ObjectIndexConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
ObjectIndex::~ObjectIndex() { close(); }

bool ObjectIndex::open(std::string& err) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->db) return true;
    std::error_code ec;
    std::filesystem::create_directories(impl_->cfg.root_directory, ec);
    const std::string path = (std::filesystem::path(impl_->cfg.root_directory) / "objects.db").string();
    if (sqlite3_open_v2(path.c_str(), &impl_->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        err = impl_->db ? sqlite3_errmsg(impl_->db) : "sqlite3_open failed";
        if (impl_->db) { sqlite3_close(impl_->db); impl_->db = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(impl_->db, impl_->cfg.busy_timeout_ms);
    std::string e;
    impl_->exec("PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;", e);
    if (!impl_->migrate(err)) { sqlite3_close(impl_->db); impl_->db = nullptr; return false; }
    return true;
}
bool ObjectIndex::is_open() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->db != nullptr; }
void ObjectIndex::close() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->db) { sqlite3_close(impl_->db); impl_->db = nullptr; }
}
void ObjectIndex::cancel() { if (sqlite3* d = impl_->intr.load()) sqlite3_interrupt(d); }

ObjectIndexResult ObjectIndex::index_region(VisualMemoryReader& frames, EmbeddingProvider& provider, int64_t frame_id,
                                            const Roi& roi, const std::string& label, const DetectionInfo* det) {
    CameraObjectSource cam(frames);
    return index_region(cam, provider, std::string(), frame_id, roi, label, det);
}

ObjectIndexResult ObjectIndex::index_region(ObjectFrameSource& src, EmbeddingProvider& provider, const std::string& kind, int64_t frame_id,
                                            const Roi& roi, const std::string& label_in, const DetectionInfo* det) {
    ObjectIndexResult r;
    const auto t0 = std::chrono::steady_clock::now();
    auto fin = [&](ObjectIndexResult::Code c, std::string m) { r.code = c; r.message = std::move(m); r.elapsed_ms = since_ms(t0); return r; };
    if (!is_open()) return fin(ObjectIndexResult::Code::NotOpen, "object index not open");
    std::string label = label_in;
    if (det && label.empty()) label = det->class_name;
    if (label.size() > impl_->cfg.max_label_bytes) return fin(ObjectIndexResult::Code::InvalidArgument, "label too long");
    for (char ch : label) if (static_cast<unsigned char>(ch) < 0x20) return fin(ObjectIndexResult::Code::InvalidArgument, "label contains control characters");
    ObjSrcFrame sf; std::string err;
    if (!src.ensure_open(err) || !src.get(kind, frame_id, sf)) return fin(ObjectIndexResult::Code::FrameNotFound, "unknown frame");
    if (!sf.image_available) return fin(ObjectIndexResult::Code::ImageUnavailable, "frame image has expired");
    std::vector<uint8_t> jpeg;
    const int jc = src.read_jpeg(kind, frame_id, jpeg, err);
    if (jc == 2) return fin(ObjectIndexResult::Code::ImageUnavailable, "frame image has expired");
    if (jc != 0) return fin(ObjectIndexResult::Code::ImageUnavailable, "frame image missing");
    detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) return fin(ObjectIndexResult::Code::DecodeFailed, err);
    const RoiCheck rc = validate_roi(roi, img.width, img.height, impl_->cfg.min_roi_px);
    if (rc != RoiCheck::Ok) return fin(ObjectIndexResult::Code::InvalidRoi, to_string(rc));
    std::vector<uint8_t> crop; int cw = 0, ch = 0;
    crop_rgb(img, roi, 0, crop, cw, ch);                       // aspect ratio preserved: embedding gets the exact crop
    ImageView v; v.data = crop.data(); v.width = cw; v.height = ch; v.stride = cw * 3; v.format = PixelFormat::RGB24;
    std::vector<float> emb;
    if (!provider.ready() || !provider.embed(v, emb)) return fin(ObjectIndexResult::Code::ProviderFailed, "embedding failed: " + provider.last_error());
    const EmbeddingSpec spec = provider.spec();
    if (detail::l2_normalize(emb, spec.dimension) != detail::VecCheck::Ok) return fin(ObjectIndexResult::Code::InvalidVector, "embedding rejected (wrong dimension, NaN or zero norm)");

    std::lock_guard<std::mutex> lk(impl_->mu);
    sqlite3* db = impl_->db;
    if (!db) return fin(ObjectIndexResult::Code::NotOpen, "object index not open");
    std::string e;
    if (!impl_->exec("BEGIN IMMEDIATE;", e)) return fin(ObjectIndexResult::Code::DatabaseError, e);
    auto rollback = [&] { std::string x; impl_->exec("ROLLBACK;", x); };
    const int64_t mid = impl_->model_id(spec, true);
    if (mid == 0) { rollback(); return fin(ObjectIndexResult::Code::DatabaseError, "cannot register model"); }
    {   // duplicate?
        Stmt q(db, "SELECT region_id,object_label FROM object_regions WHERE source_type=? AND source_kind=? AND frame_id=? AND bbox_x=? AND bbox_y=? AND bbox_width=? AND bbox_height=? AND model_id=?;");
        if (q) {
            sqlite3_bind_text(q.s, 1, sf.source_type.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(q.s, 2, sf.kind.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(q.s, 3, frame_id); sqlite3_bind_int(q.s, 4, roi.x); sqlite3_bind_int(q.s, 5, roi.y);
            sqlite3_bind_int(q.s, 6, roi.w); sqlite3_bind_int(q.s, 7, roi.h); sqlite3_bind_int64(q.s, 8, mid);
            if (sqlite3_step(q.s) == SQLITE_ROW) {
                r.region_id = sqlite3_column_int64(q.s, 0);
                const std::string old = col_text(q.s, 1);
                if (!det && !label.empty() && label != old) {     // an automatic detection never relabels a user's region
                    Stmt u(db, "UPDATE object_regions SET object_label=? WHERE region_id=?;");
                    if (u) { sqlite3_bind_text(u.s, 1, label.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int64(u.s, 2, r.region_id); sqlite3_step(u.s); }
                }
                std::string x; impl_->exec("COMMIT;", x);
                return fin(ObjectIndexResult::Code::Duplicate, "region already indexed");
            }
        }
    }
    Stmt ins(db, "INSERT INTO object_regions(frame_id,source_id,capture_utc_ms,bbox_x,bbox_y,bbox_width,bbox_height,image_width,image_height,"
                 "object_label,model_id,embedding_status,created_at,frame_sha256,index_origin,class_id,class_name,confidence,detector,detector_version,"
                 "source_type,source_kind,pop_session_id,expected_media_id,reported_media_id,playback_offset_ms,time_uncertainty_ms,provenance)"
                 " VALUES(?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!ins) { rollback(); return fin(ObjectIndexResult::Code::DatabaseError, sqlite3_errmsg(db)); }
    sqlite3_bind_int64(ins.s, 1, frame_id);
    sqlite3_bind_text(ins.s, 2, sf.source_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins.s, 3, sf.capture_utc_ms);
    sqlite3_bind_int(ins.s, 4, roi.x); sqlite3_bind_int(ins.s, 5, roi.y); sqlite3_bind_int(ins.s, 6, roi.w); sqlite3_bind_int(ins.s, 7, roi.h);
    sqlite3_bind_int(ins.s, 8, img.width); sqlite3_bind_int(ins.s, 9, img.height);
    sqlite3_bind_text(ins.s, 10, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins.s, 11, mid); sqlite3_bind_int64(ins.s, 12, now_ms());
    sqlite3_bind_text(ins.s, 13, sf.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins.s, 14, det ? "auto" : "manual", -1, SQLITE_STATIC);
    bind_source(ins.s, 20, sf);
    if (det) {
        sqlite3_bind_int(ins.s, 15, det->class_id); sqlite3_bind_text(ins.s, 16, det->class_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(ins.s, 17, det->confidence);
        sqlite3_bind_text(ins.s, 18, det->detector.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins.s, 19, det->detector_version.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(ins.s, 15); sqlite3_bind_text(ins.s, 16, "", -1, SQLITE_STATIC); sqlite3_bind_null(ins.s, 17);
        sqlite3_bind_text(ins.s, 18, "", -1, SQLITE_STATIC); sqlite3_bind_text(ins.s, 19, "", -1, SQLITE_STATIC);
    }
    if (sqlite3_step(ins.s) != SQLITE_DONE) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(ObjectIndexResult::Code::DatabaseError, m); }
    r.region_id = sqlite3_last_insert_rowid(db);
    Stmt vi(db, "INSERT INTO object_vectors(region_id,embedding) VALUES(?,?);");
    if (!vi) { rollback(); return fin(ObjectIndexResult::Code::DatabaseError, sqlite3_errmsg(db)); }
    sqlite3_bind_int64(vi.s, 1, r.region_id);
    sqlite3_bind_blob(vi.s, 2, emb.data(), static_cast<int>(emb.size() * sizeof(float)), SQLITE_TRANSIENT);
    if (sqlite3_step(vi.s) != SQLITE_DONE) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(ObjectIndexResult::Code::DatabaseError, m); }
    std::string crop_rel; int cw_out = 0, ch_out = 0; uint64_t crop_sz = 0;
    if (impl_->cfg.store_crops) {                              // independent crop JPEG (best effort)
        std::vector<uint8_t> px, cj; int w = 0, h = 0; std::string ce;
        crop_rgb(img, roi, std::max(16, impl_->cfg.crop_max_side), px, w, h);
        if (detail::encode_rgb_jpeg(px.data(), w, h, std::clamp(impl_->cfg.crop_quality, 30, 95), cj, ce)) {
            crop_rel = std::string(kCropDir) + "/" + std::to_string(r.region_id) + ".jpg";
            if (impl_->write_crop_file(crop_rel, cj)) {
                Stmt cu(db, "UPDATE object_regions SET crop_path=?,crop_bytes=?,crop_width=?,crop_height=? WHERE region_id=?;");
                if (cu) {
                    sqlite3_bind_text(cu.s, 1, crop_rel.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int64(cu.s, 2, static_cast<int64_t>(cj.size()));
                    sqlite3_bind_int(cu.s, 3, w); sqlite3_bind_int(cu.s, 4, h); sqlite3_bind_int64(cu.s, 5, r.region_id);
                    if (sqlite3_step(cu.s) == SQLITE_DONE) { cw_out = w; ch_out = h; crop_sz = cj.size(); }
                    else { impl_->remove_file(impl_->crop_abs(crop_rel)); crop_rel.clear(); }
                } else { impl_->remove_file(impl_->crop_abs(crop_rel)); crop_rel.clear(); }
            } else crop_rel.clear();
        }
    }
    if (!impl_->exec("COMMIT;", e)) {
        rollback();
        if (!crop_rel.empty()) impl_->remove_file(impl_->crop_abs(crop_rel));
        return fin(ObjectIndexResult::Code::DatabaseError, e);
    }
    (void)cw_out; (void)ch_out; (void)crop_sz;
    return fin(ObjectIndexResult::Code::Indexed, crop_rel.empty() && impl_->cfg.store_crops ? "indexed; crop image could not be stored" : "");
}

int ObjectIndex::crop_jpeg(ObjectFrameSource& src, const std::string& kind, int64_t frame_id, const Roi& roi, int max_side, int quality,
                           std::vector<uint8_t>& out, std::string& err) {
    std::vector<uint8_t> jpeg;
    if (!src.ensure_open(err)) return 3;
    const int rc = src.read_jpeg(kind, frame_id, jpeg, err);
    if (rc != 0) return rc;
    detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) return 5;
    const RoiCheck vc = validate_roi(roi, img.width, img.height, 1);
    if (vc != RoiCheck::Ok) { err = to_string(vc); return 4; }
    std::vector<uint8_t> px; int w = 0, h = 0;
    crop_rgb(img, roi, std::max(0, std::min(max_side, 1024)), px, w, h);
    return detail::encode_rgb_jpeg(px.data(), w, h, std::clamp(quality, 30, 95), out, err) ? 0 : 5;
}
int ObjectIndex::crop_jpeg(VisualMemoryReader& frames, int64_t frame_id, const Roi& roi, int max_side, int quality,
                           std::vector<uint8_t>& out, std::string& err) {
    CameraObjectSource cam(frames);
    return crop_jpeg(cam, std::string(), frame_id, roi, max_side, quality, out, err);
}

int ObjectIndex::region_crop_jpeg(const ObjectSources& srcs, int64_t region_id, int max_side, int quality,
                                  std::vector<uint8_t>& out, std::string& err) {
    ObjectRegion r;
    if (!get_region(region_id, r)) { err = "unknown region"; return 1; }
    ObjectFrameSource* fs = srcs.get(r.source_type);
    if (!fs) { err = "the " + r.source_type + " frame store is not available"; return 3; }
    return crop_jpeg(*fs, r.source_kind, r.frame_id, r.roi, max_side, quality, out, err);
}
int ObjectIndex::region_crop_jpeg(VisualMemoryReader& frames, int64_t region_id, int max_side, int quality,
                                  std::vector<uint8_t>& out, std::string& err) {
    CameraObjectSource cam(frames);
    return region_crop_jpeg(ObjectSources{&cam, nullptr}, region_id, max_side, quality, out, err);
}

ObjectSearchResult ObjectIndex::search_by_vector(const ObjectSources& avail, const std::vector<float>& v,
                                                 const EmbeddingSpec& space, const ObjectSearchOptions& o) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->scan(avail, v, space, o);
}
ObjectSearchResult ObjectIndex::search_by_vector(VisualMemoryReader* frames, const std::vector<float>& v,
                                                 const EmbeddingSpec& space, const ObjectSearchOptions& o) {
    std::unique_ptr<CameraObjectSource> cam; if (frames) cam = std::make_unique<CameraObjectSource>(*frames);
    return search_by_vector(ObjectSources{cam.get(), nullptr}, v, space, o);
}

ObjectSearchResult ObjectIndex::search_by_jpeg(const ObjectSources& avail, EmbeddingProvider& provider,
                                               const std::vector<uint8_t>& jpeg, const ObjectSearchOptions& o) {
    ObjectSearchResult res; std::string err; detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) { res.status = mk(SearchStatus::Code::InvalidArgument, "query is not a decodable JPEG: " + err); return res; }
    ImageView v; v.data = img.rgb.data(); v.width = img.width; v.height = img.height; v.stride = img.width * 3; v.format = PixelFormat::RGB24;
    std::vector<float> emb;
    if (!provider.ready() || !provider.embed(v, emb)) { res.status = mk(SearchStatus::Code::ProviderFailed, "embedding failed: " + provider.last_error()); return res; }
    return search_by_vector(avail, emb, provider.spec(), o);
}
ObjectSearchResult ObjectIndex::search_by_jpeg(VisualMemoryReader* frames, EmbeddingProvider& provider,
                                               const std::vector<uint8_t>& jpeg, const ObjectSearchOptions& o) {
    std::unique_ptr<CameraObjectSource> cam; if (frames) cam = std::make_unique<CameraObjectSource>(*frames);
    return search_by_jpeg(ObjectSources{cam.get(), nullptr}, provider, jpeg, o);
}

ObjectSearchResult ObjectIndex::search_by_roi(ObjectFrameSource& src, const ObjectSources& avail, EmbeddingProvider& provider, const std::string& kind,
                                              int64_t frame_id, const Roi& roi, const ObjectSearchOptions& o) {
    ObjectSearchResult res; std::vector<uint8_t> jpeg; std::string err;
    if (!src.ensure_open(err)) { res.status = mk(SearchStatus::Code::InvalidArgument, "frame store unavailable: " + err); return res; }
    const int rc = src.read_jpeg(kind, frame_id, jpeg, err);
    if (rc != 0) { res.status = mk(SearchStatus::Code::InvalidArgument, rc == 1 ? "unknown frame" : "frame image unavailable"); return res; }
    detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) { res.status = mk(SearchStatus::Code::InvalidArgument, err); return res; }
    const RoiCheck vc = validate_roi(roi, img.width, img.height, impl_->cfg.min_roi_px);
    if (vc != RoiCheck::Ok) { res.status = mk(SearchStatus::Code::InvalidArgument, to_string(vc)); return res; }
    std::vector<uint8_t> px; int w = 0, h = 0; crop_rgb(img, roi, 0, px, w, h);
    ImageView v; v.data = px.data(); v.width = w; v.height = h; v.stride = w * 3; v.format = PixelFormat::RGB24;
    std::vector<float> emb;
    if (!provider.ready() || !provider.embed(v, emb)) { res.status = mk(SearchStatus::Code::ProviderFailed, "embedding failed: " + provider.last_error()); return res; }
    return search_by_vector(avail, emb, provider.spec(), o);
}
ObjectSearchResult ObjectIndex::search_by_roi(VisualMemoryReader& frames, EmbeddingProvider& provider, int64_t frame_id,
                                              const Roi& roi, const ObjectSearchOptions& o) {
    CameraObjectSource cam(frames);
    return search_by_roi(cam, ObjectSources{&cam, nullptr}, provider, std::string(), frame_id, roi, o);
}

ObjectSearchResult ObjectIndex::search_by_region(VisualMemoryReader* frames, int64_t region_id, const ObjectSearchOptions& o) {
    std::unique_ptr<CameraObjectSource> cam; if (frames) cam = std::make_unique<CameraObjectSource>(*frames);
    return search_by_region(ObjectSources{cam.get(), nullptr}, region_id, o);
}

ObjectSearchResult ObjectIndex::search_by_region(const ObjectSources& frames, int64_t region_id, const ObjectSearchOptions& o) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    ObjectSearchResult res;
    if (!impl_->db) { res.status = mk(SearchStatus::Code::NotEnabled, "object index not open"); return res; }
    Stmt q(impl_->db, "SELECT r.model_id,v.embedding FROM object_regions r JOIN object_vectors v ON v.region_id=r.region_id WHERE r.region_id=? AND r.embedding_status=1;");
    if (!q) { res.status = mk(SearchStatus::Code::DatabaseError, sqlite3_errmsg(impl_->db)); return res; }
    sqlite3_bind_int64(q.s, 1, region_id);
    if (sqlite3_step(q.s) != SQLITE_ROW) { res.status = mk(SearchStatus::Code::InvalidArgument, "unknown region"); return res; }
    EmbeddingSpec spec;
    if (!impl_->model_spec(sqlite3_column_int64(q.s, 0), spec)) { res.status = mk(SearchStatus::Code::DatabaseError, "region model missing"); return res; }
    const float* p = static_cast<const float*>(sqlite3_column_blob(q.s, 1));
    if (sqlite3_column_bytes(q.s, 1) != static_cast<int>(spec.dimension * sizeof(float))) { res.status = mk(SearchStatus::Code::InvalidVector, "stored vector corrupt"); return res; }
    std::vector<float> vec(p, p + spec.dimension);
    ObjectSearchOptions oo = o; oo.exclude_region_id = region_id;
    return impl_->scan(frames, vec, spec, oo);
}

bool ObjectIndex::get_region(int64_t id, ObjectRegion& out) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    Stmt q(impl_->db, std::string("SELECT ") + kRegionCols + " FROM object_regions WHERE region_id=?;");
    if (!q) return false;
    sqlite3_bind_int64(q.s, 1, id);
    if (sqlite3_step(q.s) != SQLITE_ROW) return false;
    out = row_region(q.s);
    return true;
}

std::vector<ObjectRegion> ObjectIndex::list_regions(const ObjectFilter& f, int limit, int offset, bool newest_first, uint64_t* total) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::vector<ObjectRegion> out;
    if (!impl_->db) return out;
    limit = std::clamp(limit, 1, 500); offset = std::max(0, offset);
    std::string where = " WHERE 1";
    if (f.start_utc_ms > 0) where += " AND capture_utc_ms>=?";
    if (f.end_utc_ms > 0) where += " AND capture_utc_ms<?";
    if (!f.source_id.empty()) where += " AND source_id=?";
    if (!f.label.empty()) where += " AND object_label=?";
    if (!f.index_origin.empty()) where += " AND index_origin=?";
    if (!f.class_name.empty()) where += " AND class_name=?";
    if (!f.source_type.empty()) where += " AND source_type=?";
    if (!f.source_kind.empty()) where += " AND source_kind=?";
    if (!f.pop_session_id.empty()) where += " AND pop_session_id=?";
    if (f.frame_id > 0) where += " AND frame_id=?";
    auto bind = [&](sqlite3_stmt* s) {
        int i = 1;
        if (f.start_utc_ms > 0) sqlite3_bind_int64(s, i++, f.start_utc_ms);
        if (f.end_utc_ms > 0) sqlite3_bind_int64(s, i++, f.end_utc_ms);
        if (!f.source_id.empty()) sqlite3_bind_text(s, i++, f.source_id.c_str(), -1, SQLITE_TRANSIENT);
        if (!f.label.empty()) sqlite3_bind_text(s, i++, f.label.c_str(), -1, SQLITE_TRANSIENT);
        if (!f.index_origin.empty()) sqlite3_bind_text(s, i++, f.index_origin.c_str(), -1, SQLITE_TRANSIENT);
        if (!f.class_name.empty()) sqlite3_bind_text(s, i++, f.class_name.c_str(), -1, SQLITE_TRANSIENT);
        if (!f.source_type.empty()) sqlite3_bind_text(s, i++, f.source_type.c_str(), -1, SQLITE_TRANSIENT);
        if (!f.source_kind.empty()) sqlite3_bind_text(s, i++, f.source_kind.c_str(), -1, SQLITE_TRANSIENT);
        if (!f.pop_session_id.empty()) sqlite3_bind_text(s, i++, f.pop_session_id.c_str(), -1, SQLITE_TRANSIENT);
        if (f.frame_id > 0) sqlite3_bind_int64(s, i++, f.frame_id);
        return i;
    };
    if (total) {
        *total = 0;
        Stmt c(impl_->db, "SELECT COUNT(*) FROM object_regions" + where + ";");
        if (c) { bind(c.s); if (sqlite3_step(c.s) == SQLITE_ROW) *total = static_cast<uint64_t>(sqlite3_column_int64(c.s, 0)); }
    }
    Stmt q(impl_->db, std::string("SELECT ") + kRegionCols + " FROM object_regions" + where +
                      (newest_first ? " ORDER BY capture_utc_ms DESC,region_id DESC" : " ORDER BY capture_utc_ms,region_id") + " LIMIT ? OFFSET ?;");
    if (!q) return out;
    int i = bind(q.s); sqlite3_bind_int(q.s, i++, limit); sqlite3_bind_int(q.s, i++, offset);
    while (sqlite3_step(q.s) == SQLITE_ROW) out.push_back(row_region(q.s));
    return out;
}

bool ObjectIndex::set_label(int64_t id, const std::string& label) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db || label.size() > impl_->cfg.max_label_bytes) return false;
    for (char ch : label) if (static_cast<unsigned char>(ch) < 0x20) return false;
    Stmt u(impl_->db, "UPDATE object_regions SET object_label=? WHERE region_id=?;");
    if (!u) return false;
    sqlite3_bind_text(u.s, 1, label.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int64(u.s, 2, id);
    return sqlite3_step(u.s) == SQLITE_DONE && sqlite3_changes(impl_->db) > 0;
}

bool ObjectIndex::delete_region(int64_t id) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    std::string crop_rel;
    {
        Stmt q(impl_->db, "SELECT crop_path FROM object_regions WHERE region_id=?;");
        if (q) { sqlite3_bind_int64(q.s, 1, id); if (sqlite3_step(q.s) == SQLITE_ROW) crop_rel = col_text(q.s, 0); }
    }
    Stmt d(impl_->db, "DELETE FROM object_regions WHERE region_id=?;");
    if (!d) return false;
    sqlite3_bind_int64(d.s, 1, id);
    const bool ok = sqlite3_step(d.s) == SQLITE_DONE && sqlite3_changes(impl_->db) > 0;
    if (ok && !crop_rel.empty()) impl_->remove_file(impl_->crop_abs(crop_rel));
    return ok;
}

ObjectIndexSummary ObjectIndex::summary(const std::string& source_type) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    ObjectIndexSummary s;
    if (!impl_->db) return s;
    s.open = true; s.schema_version = impl_->user_version();
    const bool by_src = !source_type.empty();
    const std::string rw = by_src ? " WHERE source_type=?1" : "";            // regions
    const std::string ra = by_src ? " AND source_type=?1" : "";              // regions (appended)
    const std::string pw = by_src ? " WHERE source_type=?1" : "";            // progress
    const std::string pa = by_src ? " AND source_type=?1" : "";
    auto bindsrc = [&](Stmt& q) { if (by_src && q) sqlite3_bind_text(q.s, 1, source_type.c_str(), -1, SQLITE_TRANSIENT); };
    auto count = [&](const std::string& sql) { Stmt q(impl_->db, sql); bindsrc(q); return (q && sqlite3_step(q.s) == SQLITE_ROW) ? static_cast<uint64_t>(sqlite3_column_int64(q.s, 0)) : 0ull; };
    {
        Stmt q(impl_->db, "SELECT COUNT(*),COALESCE(MIN(capture_utc_ms),0),COALESCE(MAX(capture_utc_ms),0) FROM object_regions" + rw + ";"); bindsrc(q);
        if (q && sqlite3_step(q.s) == SQLITE_ROW) {
            s.regions = static_cast<uint64_t>(sqlite3_column_int64(q.s, 0));
            s.oldest_capture_utc_ms = sqlite3_column_int64(q.s, 1); s.newest_capture_utc_ms = sqlite3_column_int64(q.s, 2);
        }
    }
    {
        Stmt q(impl_->db, "SELECT COALESCE(SUM(LENGTH(v.embedding)),0) FROM object_vectors v JOIN object_regions r ON r.region_id=v.region_id" + std::string(by_src ? " WHERE r.source_type=?1" : "") + ";"); bindsrc(q);
        if (q && sqlite3_step(q.s) == SQLITE_ROW) s.vector_bytes = static_cast<uint64_t>(sqlite3_column_int64(q.s, 0));
    }
    {
        Stmt q(impl_->db, "SELECT model_id FROM object_models ORDER BY model_id;");
        std::vector<int64_t> ids;
        if (q) while (sqlite3_step(q.s) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(q.s, 0));
        for (int64_t id : ids) { EmbeddingSpec sp; if (impl_->model_spec(id, sp)) s.models.push_back(sp); }
    }
    s.manual_regions = count("SELECT COUNT(*) FROM object_regions WHERE index_origin='manual'" + ra + ";");
    s.auto_regions = count("SELECT COUNT(*) FROM object_regions WHERE index_origin='auto'" + ra + ";");
    s.crops_retained = count("SELECT COUNT(*) FROM object_regions WHERE crop_path IS NOT NULL AND crop_removed_at IS NULL" + ra + ";");
    s.crops_expired = count("SELECT COUNT(*) FROM object_regions WHERE crop_path IS NOT NULL AND crop_removed_at IS NOT NULL" + ra + ";");
    s.crop_bytes = count("SELECT COALESCE(SUM(crop_bytes),0) FROM object_regions WHERE crop_path IS NOT NULL AND crop_removed_at IS NULL" + ra + ";");
    s.frames_processed = count("SELECT COUNT(*) FROM object_frame_progress" + pw + ";");
    s.frames_with_objects = count("SELECT COUNT(*) FROM object_frame_progress WHERE objects_indexed>0" + pa + ";");
    s.frames_failed = count("SELECT COUNT(*) FROM object_frame_progress WHERE status<>'ok'" + pa + ";");
    {
        Stmt q(impl_->db, "SELECT COALESCE(MIN(capture_utc_ms),0),COALESCE(MAX(capture_utc_ms),0),COALESCE(MAX(processed_at),0) FROM object_frame_progress" + pw + ";"); bindsrc(q);
        if (q && sqlite3_step(q.s) == SQLITE_ROW) {
            s.oldest_processed_capture_utc_ms = sqlite3_column_int64(q.s, 0); s.newest_processed_capture_utc_ms = sqlite3_column_int64(q.s, 1);
            s.last_processed_utc_ms = sqlite3_column_int64(q.s, 2);
        }
    }
    {
        Stmt q(impl_->db, "SELECT class_name,COUNT(*) FROM object_regions WHERE index_origin='auto'" + ra + " GROUP BY class_name ORDER BY COUNT(*) DESC,class_name;"); bindsrc(q);
        if (q) while (sqlite3_step(q.s) == SQLITE_ROW) s.by_class.emplace_back(col_text(q.s, 0), static_cast<uint64_t>(sqlite3_column_int64(q.s, 1)));
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(std::filesystem::path(impl_->cfg.root_directory) / "objects.db", ec);
    if (!ec) s.db_bytes = static_cast<uint64_t>(sz);
    return s;
}

uint64_t ObjectIndex::prune(int64_t older_than_created_ms, uint64_t max_regions) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return 0;
    uint64_t n = 0;
    // Collect the crop files of the regions about to go (the rows cascade, the files do not).
    std::vector<std::string> doomed;
    {
        auto collect = [&](const std::string& sql, int64_t arg) {
            Stmt q(impl_->db, sql);
            if (!q) return;
            sqlite3_bind_int64(q.s, 1, arg);
            while (sqlite3_step(q.s) == SQLITE_ROW) { const std::string pth = col_text(q.s, 0); if (!pth.empty()) doomed.push_back(pth); }
        };
        if (older_than_created_ms > 0) collect("SELECT crop_path FROM object_regions WHERE created_at<? AND crop_path IS NOT NULL;", older_than_created_ms);
        // Over the count limit the AUTOMATIC regions go first (oldest indexed first); a user's manual regions are only
        // evicted once no automatic ones are left.
        if (max_regions > 0) collect("SELECT crop_path FROM object_regions WHERE crop_path IS NOT NULL AND region_id IN "
                                     "(SELECT region_id FROM object_regions ORDER BY (index_origin='manual') DESC,created_at DESC,region_id DESC LIMIT -1 OFFSET ?);", static_cast<int64_t>(max_regions));
    }
    struct Cleanup { std::vector<std::string>& v; Impl* im; ~Cleanup() { for (const auto& r : v) Impl::remove_file(im->crop_abs(r)); } } cleanup{doomed, impl_.get()};
    if (older_than_created_ms > 0) {                             // processing-progress rows follow the same age limit
        Stmt pd(impl_->db, "DELETE FROM object_frame_progress WHERE processed_at<?;");
        if (pd) { sqlite3_bind_int64(pd.s, 1, older_than_created_ms); sqlite3_step(pd.s); }
    }
    if (older_than_created_ms > 0) {
        Stmt d(impl_->db, "DELETE FROM object_regions WHERE created_at<?;");
        if (d) { sqlite3_bind_int64(d.s, 1, older_than_created_ms); if (sqlite3_step(d.s) == SQLITE_DONE) n += static_cast<uint64_t>(sqlite3_changes(impl_->db)); }
    }
    if (max_regions > 0) {
        Stmt d(impl_->db, "DELETE FROM object_regions WHERE region_id IN (SELECT region_id FROM object_regions ORDER BY (index_origin='manual') DESC,created_at DESC,region_id DESC LIMIT -1 OFFSET ?);");
        if (d) { sqlite3_bind_int64(d.s, 1, static_cast<int64_t>(max_regions)); if (sqlite3_step(d.s) == SQLITE_DONE) n += static_cast<uint64_t>(sqlite3_changes(impl_->db)); }
    }
    return n;
}

ObjectIndex::FrameResult ObjectIndex::index_frame(VisualMemoryReader& frames, EmbeddingProvider& provider, int64_t frame_id,
                                                  const std::vector<FrameRegion>& regions, FrameProgress progress,
                                                  const std::string& meta_key, const std::string& meta_value) {
    CameraObjectSource cam(frames);
    return index_frame(cam, provider, std::string(), frame_id, regions, std::move(progress), meta_key, meta_value);
}

ObjectIndex::FrameResult ObjectIndex::index_frame(ObjectFrameSource& src, EmbeddingProvider& provider, const std::string& kind, int64_t frame_id,
                                                  const std::vector<FrameRegion>& regions, FrameProgress progress,
                                                  const std::string& meta_key, const std::string& meta_value) {
    FrameResult r;
    const auto t0 = std::chrono::steady_clock::now();
    using C = ObjectIndexResult::Code;
    auto fin = [&](C c, std::string m) { r.code = c; r.message = std::move(m); r.elapsed_ms = since_ms(t0); return r; };
    if (!is_open()) return fin(C::NotOpen, "object index not open");
    ObjSrcFrame sf; std::string err; detail::DecodedImage img;
    if (!src.ensure_open(err) || !src.get(kind, frame_id, sf)) return fin(C::FrameNotFound, "unknown frame");
    if (!regions.empty()) {                                    // a frame without objects needs no image work, only its progress record
        if (!sf.image_available) return fin(C::ImageUnavailable, "frame image has expired");
        std::vector<uint8_t> jpeg;
        if (src.read_jpeg(kind, frame_id, jpeg, err) != 0) return fin(C::ImageUnavailable, "frame image missing");
        if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) return fin(C::DecodeFailed, err);
        if (!provider.ready()) return fin(C::ProviderFailed, "embedding provider not ready: " + provider.last_error());
    }
    const EmbeddingSpec spec = provider.ready() ? provider.spec() : EmbeddingSpec{};

    struct Prep { Roi roi; DetectionInfo det; std::vector<float> emb; std::string crop_rel; std::vector<uint8_t> crop; int cw = 0, ch = 0; bool wrote = false; };
    std::vector<Prep> preps;
    for (const auto& fr : regions) {
        if (validate_roi(fr.roi, img.width, img.height, impl_->cfg.min_roi_px) != RoiCheck::Ok) { ++r.invalid; continue; }
        Prep p; p.roi = fr.roi; p.det = fr.det;
        std::vector<uint8_t> px; int w = 0, h = 0;
        crop_rgb(img, fr.roi, 0, px, w, h);                    // exact box, aspect preserved: same input as a manual selection
        ImageView v; v.data = px.data(); v.width = w; v.height = h; v.stride = w * 3; v.format = PixelFormat::RGB24;
        const auto te = std::chrono::steady_clock::now();
        const bool emb_ok = provider.embed(v, p.emb);
        r.embed_ms += since_ms(te); ++r.embed_count;
        if (!emb_ok) return fin(C::ProviderFailed, "embedding failed: " + provider.last_error());
        if (detail::l2_normalize(p.emb, spec.dimension) != detail::VecCheck::Ok) return fin(C::InvalidVector, "embedding rejected (wrong dimension, NaN or zero norm)");
        if (impl_->cfg.store_crops) {
            std::vector<uint8_t> cpx; int cw = 0, ch = 0; std::string ce;
            crop_rgb(img, fr.roi, std::max(16, impl_->cfg.crop_max_side), cpx, cw, ch);
            if (detail::encode_rgb_jpeg(cpx.data(), cw, ch, std::clamp(impl_->cfg.crop_quality, 30, 95), p.crop, ce)) {
                p.cw = cw; p.ch = ch;
                // deterministic name (frame + box): known before the row exists, so the file can be written outside the transaction
                const std::string prefix = sf.source_type == "display" ? (sf.kind == "checkpoint" ? "dc" : "dh") : "f";    // never collides across sources
                p.crop_rel = std::string(kCropDir) + "/" + prefix + std::to_string(frame_id) + "_" + std::to_string(fr.roi.x) + "_" + std::to_string(fr.roi.y) + "_" +
                             std::to_string(fr.roi.w) + "_" + std::to_string(fr.roi.h) + ".jpg";
            }
        }
        preps.push_back(std::move(p));
    }
    auto cleanup_files = [&] { for (auto& p : preps) if (p.wrote) impl_->remove_file(impl_->crop_abs(p.crop_rel)); };
    for (auto& p : preps)
        if (!p.crop_rel.empty() && !std::filesystem::exists(impl_->crop_abs(p.crop_rel))) p.wrote = impl_->write_crop_file(p.crop_rel, p.crop);

    std::lock_guard<std::mutex> lk(impl_->mu);
    sqlite3* db = impl_->db;
    if (!db) { cleanup_files(); return fin(C::NotOpen, "object index not open"); }
    std::string e;
    if (!impl_->exec("BEGIN IMMEDIATE;", e)) { cleanup_files(); return fin(C::DatabaseError, e); }
    auto rollback = [&] { std::string x; impl_->exec("ROLLBACK;", x); cleanup_files(); };
    const int64_t mid = preps.empty() ? 0 : impl_->model_id(spec, true);
    if (!preps.empty() && mid == 0) { rollback(); return fin(C::DatabaseError, "cannot register model"); }
    int indexed = 0, dupes = 0;
    for (auto& p : preps) {
        {
            Stmt q(db, "SELECT region_id FROM object_regions WHERE source_type=? AND source_kind=? AND frame_id=? AND bbox_x=? AND bbox_y=? AND bbox_width=? AND bbox_height=? AND model_id=?;");
            if (!q) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(C::DatabaseError, m); }
            sqlite3_bind_text(q.s, 1, sf.source_type.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(q.s, 2, sf.kind.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(q.s, 3, frame_id); sqlite3_bind_int(q.s, 4, p.roi.x); sqlite3_bind_int(q.s, 5, p.roi.y);
            sqlite3_bind_int(q.s, 6, p.roi.w); sqlite3_bind_int(q.s, 7, p.roi.h); sqlite3_bind_int64(q.s, 8, mid);
            if (sqlite3_step(q.s) == SQLITE_ROW) { ++dupes; if (p.wrote) { impl_->remove_file(impl_->crop_abs(p.crop_rel)); p.wrote = false; } continue; }   // an existing region (manual or automatic) is never touched
        }
        Stmt ins(db, "INSERT INTO object_regions(frame_id,source_id,capture_utc_ms,bbox_x,bbox_y,bbox_width,bbox_height,image_width,image_height,"
                     "object_label,model_id,embedding_status,created_at,frame_sha256,index_origin,class_id,class_name,confidence,detector,detector_version,"
                     "crop_path,crop_bytes,crop_width,crop_height,source_type,source_kind,pop_session_id,expected_media_id,reported_media_id,"
                     "playback_offset_ms,time_uncertainty_ms,provenance) VALUES(?,?,?,?,?,?,?,?,?,?,?,1,?,?,'auto',?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
        if (!ins) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(C::DatabaseError, m); }
        const bool has_crop = p.wrote || (!p.crop_rel.empty() && std::filesystem::exists(impl_->crop_abs(p.crop_rel)));
        sqlite3_bind_int64(ins.s, 1, frame_id); sqlite3_bind_text(ins.s, 2, sf.source_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins.s, 3, sf.capture_utc_ms);
        sqlite3_bind_int(ins.s, 4, p.roi.x); sqlite3_bind_int(ins.s, 5, p.roi.y); sqlite3_bind_int(ins.s, 6, p.roi.w); sqlite3_bind_int(ins.s, 7, p.roi.h);
        sqlite3_bind_int(ins.s, 8, img.width); sqlite3_bind_int(ins.s, 9, img.height);
        sqlite3_bind_text(ins.s, 10, p.det.class_name.c_str(), -1, SQLITE_TRANSIENT);                     // label defaults to the class
        sqlite3_bind_int64(ins.s, 11, mid); sqlite3_bind_int64(ins.s, 12, now_ms());
        sqlite3_bind_text(ins.s, 13, sf.sha256.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins.s, 14, p.det.class_id); sqlite3_bind_text(ins.s, 15, p.det.class_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(ins.s, 16, p.det.confidence);
        sqlite3_bind_text(ins.s, 17, p.det.detector.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(ins.s, 18, p.det.detector_version.c_str(), -1, SQLITE_TRANSIENT);
        if (has_crop) {
            sqlite3_bind_text(ins.s, 19, p.crop_rel.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int64(ins.s, 20, static_cast<int64_t>(p.crop.size()));
            sqlite3_bind_int(ins.s, 21, p.cw); sqlite3_bind_int(ins.s, 22, p.ch);
        } else { sqlite3_bind_null(ins.s, 19); sqlite3_bind_int64(ins.s, 20, 0); sqlite3_bind_int(ins.s, 21, 0); sqlite3_bind_int(ins.s, 22, 0); }
        bind_source(ins.s, 23, sf);
        if (sqlite3_step(ins.s) != SQLITE_DONE) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(C::DatabaseError, m); }
        const int64_t rid = sqlite3_last_insert_rowid(db);
        Stmt vi(db, "INSERT INTO object_vectors(region_id,embedding) VALUES(?,?);");
        if (!vi) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(C::DatabaseError, m); }
        sqlite3_bind_int64(vi.s, 1, rid); sqlite3_bind_blob(vi.s, 2, p.emb.data(), static_cast<int>(p.emb.size() * sizeof(float)), SQLITE_TRANSIENT);
        if (sqlite3_step(vi.s) != SQLITE_DONE) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(C::DatabaseError, m); }
        ++indexed;
    }
    {   // progress + cursor ride in the same commit
        progress.objects_indexed = indexed + dupes; progress.status = "ok";
        if (progress.processed_utc_ms <= 0) progress.processed_utc_ms = now_ms();
        Stmt u(db, "INSERT OR REPLACE INTO object_frame_progress(source_type,source_kind,frame_id,detector,detector_version,source_id,capture_utc_ms,status,"
                   "objects_found,objects_indexed,threshold,error,processed_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);");
        if (!u) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(C::DatabaseError, m); }
        sqlite3_bind_text(u.s, 1, sf.source_type.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(u.s, 2, sf.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(u.s, 3, frame_id); sqlite3_bind_text(u.s, 4, progress.detector.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u.s, 5, progress.detector_version.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u.s, 6, sf.source_id.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int64(u.s, 7, sf.capture_utc_ms);
        sqlite3_bind_text(u.s, 8, progress.status.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int(u.s, 9, progress.objects_found);
        sqlite3_bind_int(u.s, 10, progress.objects_indexed); sqlite3_bind_double(u.s, 11, progress.threshold);
        sqlite3_bind_text(u.s, 12, "", -1, SQLITE_STATIC); sqlite3_bind_int64(u.s, 13, progress.processed_utc_ms);
        if (sqlite3_step(u.s) != SQLITE_DONE) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(C::DatabaseError, m); }
    }
    if (!meta_key.empty()) {
        Stmt m(db, "INSERT OR REPLACE INTO object_meta(key,value) VALUES(?,?);");
        if (!m) { const std::string x = sqlite3_errmsg(db); rollback(); return fin(C::DatabaseError, x); }
        sqlite3_bind_text(m.s, 1, meta_key.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(m.s, 2, meta_value.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(m.s) != SQLITE_DONE) { const std::string x = sqlite3_errmsg(db); rollback(); return fin(C::DatabaseError, x); }
    }
    if (!impl_->exec("COMMIT;", e)) { rollback(); return fin(C::DatabaseError, e); }
    r.indexed = indexed; r.duplicates = dupes;
    return fin(C::Indexed, "");
}

int ObjectIndex::stored_crop_jpeg(int64_t region_id, std::vector<uint8_t>& out, std::string& err) {
    std::string rel; bool removed = false;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->db) { err = "object index not open"; return 1; }
        Stmt q(impl_->db, "SELECT crop_path,crop_removed_at FROM object_regions WHERE region_id=?;");
        if (!q) { err = sqlite3_errmsg(impl_->db); return 1; }
        sqlite3_bind_int64(q.s, 1, region_id);
        if (sqlite3_step(q.s) != SQLITE_ROW) { err = "unknown region"; return 1; }
        rel = col_text(q.s, 0); removed = sqlite3_column_type(q.s, 1) != SQLITE_NULL;
    }
    if (rel.empty()) { err = "no crop image was stored for this object"; return 2; }
    if (removed) { err = "the crop image has expired"; return 2; }
    std::error_code ec;
    const auto path = impl_->crop_abs(rel);
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz == 0 || sz > (8u << 20)) { err = "crop image file missing"; return 3; }
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) { err = "crop image file unreadable"; return 3; }
    out.resize(static_cast<size_t>(sz));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) { out.clear(); err = "crop image file unreadable"; return 3; }
    return 0;
}

bool ObjectIndex::get_frame_progress(int64_t frame_id, const std::string& detector, const std::string& version, FrameProgress& out) {
    return get_frame_progress("camera", std::string(), frame_id, detector, version, out);
}
bool ObjectIndex::get_frame_progress(const std::string& source_type, const std::string& kind, int64_t frame_id, const std::string& detector,
                                     const std::string& version, FrameProgress& out) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    Stmt q(impl_->db, "SELECT source_id,capture_utc_ms,status,objects_found,objects_indexed,threshold,error,processed_at FROM object_frame_progress "
                      "WHERE source_type=? AND source_kind=? AND frame_id=? AND detector=? AND detector_version=?;");
    if (!q) return false;
    sqlite3_bind_text(q.s, 1, source_type.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(q.s, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(q.s, 3, frame_id); sqlite3_bind_text(q.s, 4, detector.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q.s, 5, version.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q.s) != SQLITE_ROW) return false;
    out = FrameProgress{}; out.source_type = source_type; out.source_kind = kind; out.frame_id = frame_id; out.detector = detector; out.detector_version = version;
    out.source_id = col_text(q.s, 0); out.capture_utc_ms = sqlite3_column_int64(q.s, 1); out.status = col_text(q.s, 2);
    out.objects_found = sqlite3_column_int(q.s, 3); out.objects_indexed = sqlite3_column_int(q.s, 4);
    out.threshold = static_cast<float>(sqlite3_column_double(q.s, 5)); out.error = col_text(q.s, 6);
    out.processed_utc_ms = sqlite3_column_int64(q.s, 7);
    return true;
}

bool ObjectIndex::record_frame_progress(const FrameProgress& p) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    Stmt u(impl_->db, "INSERT OR REPLACE INTO object_frame_progress(source_type,source_kind,frame_id,detector,detector_version,source_id,capture_utc_ms,status,"
                      "objects_found,objects_indexed,threshold,error,processed_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!u) return false;
    sqlite3_bind_text(u.s, 1, p.source_type.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(u.s, 2, p.source_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(u.s, 3, p.frame_id); sqlite3_bind_text(u.s, 4, p.detector.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(u.s, 5, p.detector_version.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(u.s, 6, p.source_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(u.s, 7, p.capture_utc_ms); sqlite3_bind_text(u.s, 8, p.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(u.s, 9, p.objects_found); sqlite3_bind_int(u.s, 10, p.objects_indexed); sqlite3_bind_double(u.s, 11, p.threshold);
    sqlite3_bind_text(u.s, 12, p.error.substr(0, 300).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(u.s, 13, p.processed_utc_ms > 0 ? p.processed_utc_ms : now_ms());
    return sqlite3_step(u.s) == SQLITE_DONE;
}

bool ObjectIndex::get_meta(const std::string& key, std::string& value) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    Stmt q(impl_->db, "SELECT value FROM object_meta WHERE key=?;");
    if (!q) return false;
    sqlite3_bind_text(q.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q.s) != SQLITE_ROW) return false;
    value = col_text(q.s, 0);
    return true;
}

bool ObjectIndex::set_meta(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->db) return false;
    Stmt u(impl_->db, "INSERT OR REPLACE INTO object_meta(key,value) VALUES(?,?);");
    if (!u) return false;
    sqlite3_bind_text(u.s, 1, key.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(u.s, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(u.s) == SQLITE_DONE;
}

CropPruneResult ObjectIndex::prune_crops(int64_t older_than_created_ms, uint64_t max_bytes) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    CropPruneResult res;
    if (!impl_->db) return res;
    struct Row { int64_t id; std::string rel; uint64_t bytes; int64_t created; };
    std::vector<Row> rows;                                   // retained crops, oldest indexed first
    {
        Stmt q(impl_->db, "SELECT region_id,crop_path,crop_bytes,created_at FROM object_regions WHERE crop_path IS NOT NULL AND crop_removed_at IS NULL "
                          "ORDER BY created_at,region_id;");
        if (!q) return res;
        while (sqlite3_step(q.s) == SQLITE_ROW)
            rows.push_back({sqlite3_column_int64(q.s, 0), col_text(q.s, 1), static_cast<uint64_t>(sqlite3_column_int64(q.s, 2)), sqlite3_column_int64(q.s, 3)});
    }
    uint64_t total = 0; for (const auto& r : rows) total += r.bytes;
    const int64_t t = now_ms();
    for (const auto& r : rows) {
        const bool too_old = older_than_created_ms > 0 && r.created < older_than_created_ms;
        const bool over = max_bytes > 0 && total > max_bytes;
        if (!too_old && !over) break;
        Stmt u(impl_->db, "UPDATE object_regions SET crop_removed_at=? WHERE region_id=?;");
        if (!u) break;
        sqlite3_bind_int64(u.s, 1, t); sqlite3_bind_int64(u.s, 2, r.id);
        if (sqlite3_step(u.s) != SQLITE_DONE) break;
        impl_->remove_file(impl_->crop_abs(r.rel));
        total -= std::min(total, r.bytes); ++res.removed; res.bytes_freed += r.bytes;
    }
    return res;
}

} // namespace visualmemory
