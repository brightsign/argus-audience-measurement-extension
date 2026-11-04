// SPDX-License-Identifier: MIT
#include "visualmemory/object_index.h"

#include "embedding_math.h"
#include "jpeg_decoder.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <queue>

namespace visualmemory {

namespace {
constexpr int kSchemaVersion = 1;

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
    "object_label,model_id,embedding_status,crop_path,created_at,frame_sha256";

ObjectRegion row_region(sqlite3_stmt* s) {
    ObjectRegion r;
    r.region_id = sqlite3_column_int64(s, 0); r.frame_id = sqlite3_column_int64(s, 1);
    r.source_id = col_text(s, 2); r.capture_utc_ms = sqlite3_column_int64(s, 3);
    r.roi = {sqlite3_column_int(s, 4), sqlite3_column_int(s, 5), sqlite3_column_int(s, 6), sqlite3_column_int(s, 7)};
    r.image_width = sqlite3_column_int(s, 8); r.image_height = sqlite3_column_int(s, 9);
    r.label = col_text(s, 10); r.model_id = sqlite3_column_int64(s, 11);
    r.embedding_status = sqlite3_column_int(s, 12);
    r.has_crop_file = sqlite3_column_type(s, 13) != SQLITE_NULL;
    r.created_utc_ms = sqlite3_column_int64(s, 14); r.frame_sha256 = col_text(s, 15);
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
    uint64_t count_all() {
        Stmt q(db, "SELECT COUNT(*) FROM object_regions;");
        return (q && sqlite3_step(q.s) == SQLITE_ROW) ? static_cast<uint64_t>(sqlite3_column_int64(q.s, 0)) : 0;
    }

    ObjectSearchResult scan(VisualMemoryReader* frames, const std::vector<float>& qin, const EmbeddingSpec& space,
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
        if (o.exclude_region_id > 0) where += " AND r.region_id<>?";
        Stmt st(db, "SELECT r.region_id,v.embedding FROM object_regions r JOIN object_vectors v ON v.region_id=r.region_id" + where + ";");
        if (!st) return fin(SearchStatus::Code::DatabaseError, sqlite3_errmsg(db));
        int bi = 1; sqlite3_bind_int64(st.s, bi++, mid);
        if (o.start_utc_ms > 0) sqlite3_bind_int64(st.s, bi++, o.start_utc_ms);
        if (o.end_utc_ms > 0) sqlite3_bind_int64(st.s, bi++, o.end_utc_ms);
        if (!o.source_id.empty()) sqlite3_bind_text(st.s, bi++, o.source_id.c_str(), -1, SQLITE_TRANSIENT);
        if (!o.label.empty()) sqlite3_bind_text(st.s, bi++, o.label.c_str(), -1, SQLITE_TRANSIENT);
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
            if (frames && frames->is_open()) {
                FrameDetail fd;
                if (frames->get_frame(h.region.frame_id, fd)) { h.image_available = fd.record.frame.image_available; h.image_exists = fd.record.image_exists; }
            }
            if (o.require_image && !h.image_exists) continue;
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
                                            const Roi& roi, const std::string& label_in) {
    ObjectIndexResult r;
    const auto t0 = std::chrono::steady_clock::now();
    auto fin = [&](ObjectIndexResult::Code c, std::string m) { r.code = c; r.message = std::move(m); r.elapsed_ms = since_ms(t0); return r; };
    if (!is_open()) return fin(ObjectIndexResult::Code::NotOpen, "object index not open");
    std::string label = label_in;
    if (label.size() > impl_->cfg.max_label_bytes) return fin(ObjectIndexResult::Code::InvalidArgument, "label too long");
    for (char ch : label) if (static_cast<unsigned char>(ch) < 0x20) return fin(ObjectIndexResult::Code::InvalidArgument, "label contains control characters");
    FrameDetail fd;
    if (!frames.get_frame(frame_id, fd)) return fin(ObjectIndexResult::Code::FrameNotFound, "unknown frame");
    if (!fd.record.frame.image_available) return fin(ObjectIndexResult::Code::ImageUnavailable, "frame image has expired");
    std::vector<uint8_t> jpeg; std::string err;
    const int jc = frames.read_jpeg(frame_id, jpeg, err);
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
        Stmt q(db, "SELECT region_id,object_label FROM object_regions WHERE frame_id=? AND bbox_x=? AND bbox_y=? AND bbox_width=? AND bbox_height=? AND model_id=?;");
        if (q) {
            sqlite3_bind_int64(q.s, 1, frame_id); sqlite3_bind_int(q.s, 2, roi.x); sqlite3_bind_int(q.s, 3, roi.y);
            sqlite3_bind_int(q.s, 4, roi.w); sqlite3_bind_int(q.s, 5, roi.h); sqlite3_bind_int64(q.s, 6, mid);
            if (sqlite3_step(q.s) == SQLITE_ROW) {
                r.region_id = sqlite3_column_int64(q.s, 0);
                const std::string old = col_text(q.s, 1);
                if (!label.empty() && label != old) {
                    Stmt u(db, "UPDATE object_regions SET object_label=? WHERE region_id=?;");
                    if (u) { sqlite3_bind_text(u.s, 1, label.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int64(u.s, 2, r.region_id); sqlite3_step(u.s); }
                }
                std::string x; impl_->exec("COMMIT;", x);
                return fin(ObjectIndexResult::Code::Duplicate, "region already indexed");
            }
        }
    }
    Stmt ins(db, "INSERT INTO object_regions(frame_id,source_id,capture_utc_ms,bbox_x,bbox_y,bbox_width,bbox_height,image_width,image_height,"
                 "object_label,model_id,embedding_status,created_at,frame_sha256) VALUES(?,?,?,?,?,?,?,?,?,?,?,1,?,?);");
    if (!ins) { rollback(); return fin(ObjectIndexResult::Code::DatabaseError, sqlite3_errmsg(db)); }
    const FrameInfo& fi = fd.record.frame;
    sqlite3_bind_int64(ins.s, 1, frame_id);
    sqlite3_bind_text(ins.s, 2, fi.metadata.source_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins.s, 3, fi.metadata.capture_utc_ms);
    sqlite3_bind_int(ins.s, 4, roi.x); sqlite3_bind_int(ins.s, 5, roi.y); sqlite3_bind_int(ins.s, 6, roi.w); sqlite3_bind_int(ins.s, 7, roi.h);
    sqlite3_bind_int(ins.s, 8, img.width); sqlite3_bind_int(ins.s, 9, img.height);
    sqlite3_bind_text(ins.s, 10, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins.s, 11, mid); sqlite3_bind_int64(ins.s, 12, now_ms());
    sqlite3_bind_text(ins.s, 13, fi.sha256_hex.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(ins.s) != SQLITE_DONE) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(ObjectIndexResult::Code::DatabaseError, m); }
    r.region_id = sqlite3_last_insert_rowid(db);
    Stmt vi(db, "INSERT INTO object_vectors(region_id,embedding) VALUES(?,?);");
    if (!vi) { rollback(); return fin(ObjectIndexResult::Code::DatabaseError, sqlite3_errmsg(db)); }
    sqlite3_bind_int64(vi.s, 1, r.region_id);
    sqlite3_bind_blob(vi.s, 2, emb.data(), static_cast<int>(emb.size() * sizeof(float)), SQLITE_TRANSIENT);
    if (sqlite3_step(vi.s) != SQLITE_DONE) { const std::string m = sqlite3_errmsg(db); rollback(); return fin(ObjectIndexResult::Code::DatabaseError, m); }
    if (!impl_->exec("COMMIT;", e)) { rollback(); return fin(ObjectIndexResult::Code::DatabaseError, e); }
    return fin(ObjectIndexResult::Code::Indexed, "");
}

int ObjectIndex::crop_jpeg(VisualMemoryReader& frames, int64_t frame_id, const Roi& roi, int max_side, int quality,
                           std::vector<uint8_t>& out, std::string& err) {
    std::vector<uint8_t> jpeg;
    const int rc = frames.read_jpeg(frame_id, jpeg, err);
    if (rc != 0) return rc;
    detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) return 5;
    const RoiCheck vc = validate_roi(roi, img.width, img.height, 1);
    if (vc != RoiCheck::Ok) { err = to_string(vc); return 4; }
    std::vector<uint8_t> px; int w = 0, h = 0;
    crop_rgb(img, roi, std::max(0, std::min(max_side, 1024)), px, w, h);
    return detail::encode_rgb_jpeg(px.data(), w, h, std::clamp(quality, 30, 95), out, err) ? 0 : 5;
}

int ObjectIndex::region_crop_jpeg(VisualMemoryReader& frames, int64_t region_id, int max_side, int quality,
                                  std::vector<uint8_t>& out, std::string& err) {
    ObjectRegion r;
    if (!get_region(region_id, r)) { err = "unknown region"; return 1; }
    return crop_jpeg(frames, r.frame_id, r.roi, max_side, quality, out, err);
}

ObjectSearchResult ObjectIndex::search_by_vector(VisualMemoryReader* frames, const std::vector<float>& v,
                                                 const EmbeddingSpec& space, const ObjectSearchOptions& o) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->scan(frames, v, space, o);
}

ObjectSearchResult ObjectIndex::search_by_jpeg(VisualMemoryReader* frames, EmbeddingProvider& provider,
                                               const std::vector<uint8_t>& jpeg, const ObjectSearchOptions& o) {
    ObjectSearchResult res; std::string err; detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) { res.status = mk(SearchStatus::Code::InvalidArgument, "query is not a decodable JPEG: " + err); return res; }
    ImageView v; v.data = img.rgb.data(); v.width = img.width; v.height = img.height; v.stride = img.width * 3; v.format = PixelFormat::RGB24;
    std::vector<float> emb;
    if (!provider.ready() || !provider.embed(v, emb)) { res.status = mk(SearchStatus::Code::ProviderFailed, "embedding failed: " + provider.last_error()); return res; }
    return search_by_vector(frames, emb, provider.spec(), o);
}

ObjectSearchResult ObjectIndex::search_by_roi(VisualMemoryReader& frames, EmbeddingProvider& provider, int64_t frame_id,
                                              const Roi& roi, const ObjectSearchOptions& o) {
    ObjectSearchResult res; std::vector<uint8_t> jpeg; std::string err;
    const int rc = frames.read_jpeg(frame_id, jpeg, err);
    if (rc != 0) { res.status = mk(SearchStatus::Code::InvalidArgument, rc == 1 ? "unknown frame" : "frame image unavailable"); return res; }
    detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) { res.status = mk(SearchStatus::Code::InvalidArgument, err); return res; }
    const RoiCheck vc = validate_roi(roi, img.width, img.height, impl_->cfg.min_roi_px);
    if (vc != RoiCheck::Ok) { res.status = mk(SearchStatus::Code::InvalidArgument, to_string(vc)); return res; }
    std::vector<uint8_t> px; int w = 0, h = 0; crop_rgb(img, roi, 0, px, w, h);
    ImageView v; v.data = px.data(); v.width = w; v.height = h; v.stride = w * 3; v.format = PixelFormat::RGB24;
    std::vector<float> emb;
    if (!provider.ready() || !provider.embed(v, emb)) { res.status = mk(SearchStatus::Code::ProviderFailed, "embedding failed: " + provider.last_error()); return res; }
    return search_by_vector(&frames, emb, provider.spec(), o);
}

ObjectSearchResult ObjectIndex::search_by_region(VisualMemoryReader* frames, int64_t region_id, const ObjectSearchOptions& o) {
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
    if (f.frame_id > 0) where += " AND frame_id=?";
    auto bind = [&](sqlite3_stmt* s) {
        int i = 1;
        if (f.start_utc_ms > 0) sqlite3_bind_int64(s, i++, f.start_utc_ms);
        if (f.end_utc_ms > 0) sqlite3_bind_int64(s, i++, f.end_utc_ms);
        if (!f.source_id.empty()) sqlite3_bind_text(s, i++, f.source_id.c_str(), -1, SQLITE_TRANSIENT);
        if (!f.label.empty()) sqlite3_bind_text(s, i++, f.label.c_str(), -1, SQLITE_TRANSIENT);
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
    Stmt d(impl_->db, "DELETE FROM object_regions WHERE region_id=?;");
    if (!d) return false;
    sqlite3_bind_int64(d.s, 1, id);
    return sqlite3_step(d.s) == SQLITE_DONE && sqlite3_changes(impl_->db) > 0;
}

ObjectIndexSummary ObjectIndex::summary() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    ObjectIndexSummary s;
    if (!impl_->db) return s;
    s.open = true; s.schema_version = impl_->user_version();
    {
        Stmt q(impl_->db, "SELECT COUNT(*),COALESCE(MIN(capture_utc_ms),0),COALESCE(MAX(capture_utc_ms),0) FROM object_regions;");
        if (q && sqlite3_step(q.s) == SQLITE_ROW) {
            s.regions = static_cast<uint64_t>(sqlite3_column_int64(q.s, 0));
            s.oldest_capture_utc_ms = sqlite3_column_int64(q.s, 1); s.newest_capture_utc_ms = sqlite3_column_int64(q.s, 2);
        }
    }
    {
        Stmt q(impl_->db, "SELECT COALESCE(SUM(LENGTH(embedding)),0) FROM object_vectors;");
        if (q && sqlite3_step(q.s) == SQLITE_ROW) s.vector_bytes = static_cast<uint64_t>(sqlite3_column_int64(q.s, 0));
    }
    {
        Stmt q(impl_->db, "SELECT model_id FROM object_models ORDER BY model_id;");
        std::vector<int64_t> ids;
        if (q) while (sqlite3_step(q.s) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(q.s, 0));
        for (int64_t id : ids) { EmbeddingSpec sp; if (impl_->model_spec(id, sp)) s.models.push_back(sp); }
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
    if (older_than_created_ms > 0) {
        Stmt d(impl_->db, "DELETE FROM object_regions WHERE created_at<?;");
        if (d) { sqlite3_bind_int64(d.s, 1, older_than_created_ms); if (sqlite3_step(d.s) == SQLITE_DONE) n += static_cast<uint64_t>(sqlite3_changes(impl_->db)); }
    }
    if (max_regions > 0) {
        Stmt d(impl_->db, "DELETE FROM object_regions WHERE region_id IN (SELECT region_id FROM object_regions ORDER BY created_at DESC,region_id DESC LIMIT -1 OFFSET ?);");
        if (d) { sqlite3_bind_int64(d.s, 1, static_cast<int64_t>(max_regions)); if (sqlite3_step(d.s) == SQLITE_DONE) n += static_cast<uint64_t>(sqlite3_changes(impl_->db)); }
    }
    return n;
}

} // namespace visualmemory
