// SPDX-License-Identifier: MIT
#include "visualmemory/reader.h"

#include "embedding_math.h"
#include "file_store.h"
#include "jpeg_decoder.h"
#include "search_core.h"
#include "sqlite_store.h"
#include "vector_store.h"
#include "visualmemory/latency_ring.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <mutex>

namespace visualmemory {

namespace {
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
    return a.model_name == b.model_name && a.model_version == b.model_version &&
           a.model_sha256 == b.model_sha256 && a.preprocessing == b.preprocessing &&
           a.dimension == b.dimension && a.dtype == b.dtype && a.metric == b.metric;
}
SearchStatus mk(SearchStatus::Code c, std::string m) { SearchStatus s; s.code = c; s.message = std::move(m); return s; }

const char* kFrameCols =
    "f.id,f.source_id,f.source_type,f.device_id,f.capture_utc_ms,f.pts_ns,f.source_sequence,"
    "f.stored_width,f.stored_height,f.relative_jpeg_path,f.jpeg_size_bytes,f.jpeg_sha256,f.created_utc_ms,"
    "f.image_available,EXISTS(SELECT 1 FROM frame_embeddings e WHERE e.frame_id=f.id AND e.status=1)";

FrameRecord row_to_record(sqlite3_stmt* st) {
    FrameRecord r;
    FrameInfo& fi = r.frame;
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
    r.has_embedding = sqlite3_column_int(st, 14) != 0;
    return r;
}
} // namespace

struct VisualMemoryReader::Impl {
    ReaderConfig cfg;
    detail::SqliteStore db;
    std::unique_ptr<detail::VectorStore> vs;
    std::unique_ptr<detail::FileStore> files;
    std::shared_ptr<EmbeddingProvider> provider;
    bool opened = false;

    explicit Impl(ReaderConfig c) : cfg(std::move(c)) {}

    bool image_exists(const FrameInfo& fi) const {
        if (!fi.image_available || !files || !files->is_safe_relative(fi.relative_jpeg_path)) return false;
        std::error_code ec;
        return std::filesystem::exists(files->absolute(fi.relative_jpeg_path), ec);
    }

    // The stored model whose space equals the provider's; else invalid.
    bool model_for_provider(detail::ModelInfo& out) {
        if (!provider) return false;
        const EmbeddingSpec ps = provider->spec();
        for (auto& m : vs->models()) if (same_space(m.spec, ps)) { out = m; return true; }
        return false;
    }
};

VisualMemoryReader::VisualMemoryReader(ReaderConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
VisualMemoryReader::~VisualMemoryReader() { close(); }

bool VisualMemoryReader::open(std::string& err) {
    if (impl_->opened) return true;
    const auto db_path = (std::filesystem::path(impl_->cfg.root_directory) / "visual_memory.db").string();
    std::error_code ec;
    if (!std::filesystem::exists(db_path, ec)) { err = "database not found: " + db_path; return false; }
    if (!impl_->db.open_readonly(db_path, impl_->cfg.busy_timeout_ms, err)) return false;
    impl_->vs = std::make_unique<detail::VectorStore>(impl_->db);
    impl_->files = std::make_unique<detail::FileStore>(impl_->cfg.root_directory);   // no init(): never creates dirs
    impl_->opened = true;
    return true;
}

bool VisualMemoryReader::is_open() const { return impl_->opened; }

void VisualMemoryReader::close() {
    if (!impl_->opened) return;
    impl_->opened = false;
    impl_->db.close();
}

void VisualMemoryReader::set_embedding_provider(std::shared_ptr<EmbeddingProvider> p) { impl_->provider = std::move(p); }

void VisualMemoryReader::cancel_search() { impl_->db.interrupt(); }

std::string VisualMemoryReader::absolute_path(const std::string& rel) const {
    return impl_->files ? impl_->files->absolute(rel) : std::string{};
}

StoreSummary VisualMemoryReader::summary() {
    StoreSummary s;
    if (!impl_->opened) return s;
    s.open = true;
    s.schema_version = impl_->db.schema_version();
    s.sqlite_version = impl_->db.sqlite_version();
    s.vec_version = impl_->db.vec_available() ? impl_->db.vec_version() : std::string{};
    impl_->db.with_db([&](sqlite3* db) -> int {
        if (!db) return 0;
        {
            Stmt q(db, "SELECT COUNT(*),COALESCE(SUM(image_available),0),"
                       "COALESCE(SUM(CASE WHEN image_available=1 THEN jpeg_size_bytes ELSE 0 END),0),"
                       "COALESCE(MIN(capture_utc_ms),0),COALESCE(MAX(capture_utc_ms),0) FROM frames;");
            if (q && sqlite3_step(q.s) == SQLITE_ROW) {
                s.frames = static_cast<uint64_t>(sqlite3_column_int64(q.s, 0));
                s.frames_with_image = static_cast<uint64_t>(sqlite3_column_int64(q.s, 1));
                s.jpeg_bytes = static_cast<uint64_t>(sqlite3_column_int64(q.s, 2));
                s.oldest_capture_utc_ms = sqlite3_column_int64(q.s, 3);
                s.newest_capture_utc_ms = sqlite3_column_int64(q.s, 4);
            }
        }
        {
            Stmt q(db, "SELECT COALESCE(MAX(created_utc_ms),0) FROM frame_embeddings WHERE status=1;");
            if (q && sqlite3_step(q.s) == SQLITE_ROW) s.latest_embedding_created_utc_ms = sqlite3_column_int64(q.s, 0);
        }
        {
            Stmt q(db, "SELECT DISTINCT source_id FROM frames ORDER BY source_id LIMIT 64;");
            if (q) while (sqlite3_step(q.s) == SQLITE_ROW) s.sources.push_back(col_text(q.s, 0));
        }
        return 0;
    });
    s.vector_bytes = impl_->vs->total_vector_bytes();
    for (auto& m : impl_->vs->models()) {
        ModelSummary ms; ms.spec = m.spec; ms.model_id = m.model_id; ms.backend = m.backend;
        ms.vectors_ok = impl_->vs->count(m); ms.vectors_failed = impl_->vs->count_failed(m);
        s.embeddings_ok += ms.vectors_ok; s.embeddings_failed += ms.vectors_failed;
        s.models.push_back(std::move(ms));
    }
    std::error_code ec;
    const auto db_path = std::filesystem::path(impl_->cfg.root_directory) / "visual_memory.db";
    const auto sz = std::filesystem::file_size(db_path, ec);
    if (!ec) s.db_bytes = static_cast<uint64_t>(sz);
    return s;
}

std::vector<FrameRecord> VisualMemoryReader::list_frames(const FrameFilter& f, int limit, int offset,
                                                         bool newest_first, uint64_t* total) {
    std::vector<FrameRecord> out;
    if (!impl_->opened) return out;
    limit = std::max(1, std::min(limit, 500));
    offset = std::max(0, offset);
    std::string where = " WHERE 1";
    if (f.start_utc_ms > 0) where += " AND f.capture_utc_ms>=?";
    if (f.end_utc_ms > 0)   where += " AND f.capture_utc_ms<?";
    if (!f.source_id.empty()) where += " AND f.source_id=?";
    auto bind_filter = [&](sqlite3_stmt* st) {
        int i = 1;
        if (f.start_utc_ms > 0) sqlite3_bind_int64(st, i++, f.start_utc_ms);
        if (f.end_utc_ms > 0)   sqlite3_bind_int64(st, i++, f.end_utc_ms);
        if (!f.source_id.empty()) sqlite3_bind_text(st, i++, f.source_id.c_str(), -1, SQLITE_TRANSIENT);
        return i;
    };
    impl_->db.with_db([&](sqlite3* db) -> int {
        if (!db) return 0;
        if (total) {
            *total = 0;
            Stmt c(db, "SELECT COUNT(*) FROM frames f" + where + ";");
            if (c) { bind_filter(c.s); if (sqlite3_step(c.s) == SQLITE_ROW) *total = static_cast<uint64_t>(sqlite3_column_int64(c.s, 0)); }
        }
        Stmt q(db, std::string("SELECT ") + kFrameCols + " FROM frames f" + where +
                   (newest_first ? " ORDER BY f.capture_utc_ms DESC,f.id DESC" : " ORDER BY f.capture_utc_ms,f.id") +
                   " LIMIT ? OFFSET ?;");
        if (!q) return 0;
        int i = bind_filter(q.s);
        sqlite3_bind_int(q.s, i++, limit);
        sqlite3_bind_int(q.s, i++, offset);
        while (sqlite3_step(q.s) == SQLITE_ROW) out.push_back(row_to_record(q.s));
        return 0;
    });
    for (auto& r : out) r.image_exists = impl_->image_exists(r.frame);
    return out;
}

bool VisualMemoryReader::get_frame(int64_t id, FrameDetail& out) {
    if (!impl_->opened) return false;
    bool found = false;
    impl_->db.with_db([&](sqlite3* db) -> int {
        if (!db) return 0;
        Stmt q(db, std::string("SELECT ") + kFrameCols + " FROM frames f WHERE f.id=?;");
        if (!q) return 0;
        sqlite3_bind_int64(q.s, 1, id);
        if (sqlite3_step(q.s) == SQLITE_ROW) { out.record = row_to_record(q.s); found = true; }
        return 0;
    });
    if (!found) return false;
    out.record.image_exists = impl_->image_exists(out.record.frame);
    out.embeddings.clear();
    const auto models = impl_->vs->models();
    for (const auto& e : impl_->vs->frame_embeddings(id)) {
        EmbeddingRecord r;
        r.model_id = e.model_id; r.ok = (e.status == 1); r.created_utc_ms = e.created_utc_ms; r.error = e.error;
        for (const auto& m : models) if (m.model_id == e.model_id) {
            r.model_name = m.spec.model_name; r.model_version = m.spec.model_version;
            r.model_sha256 = m.spec.model_sha256; r.preprocessing = m.spec.preprocessing;
            r.dimension = m.spec.dimension;
        }
        out.embeddings.push_back(std::move(r));
    }
    return true;
}

int VisualMemoryReader::read_jpeg(int64_t id, std::vector<uint8_t>& out, std::string& err) {
    FrameDetail d;
    if (!get_frame(id, d)) return 1;
    if (!d.record.frame.image_available) return 2;
    const std::string& rel = d.record.frame.relative_jpeg_path;
    if (!impl_->files->is_safe_relative(rel)) { err = "unsafe path in database"; return 3; }
    if (!detail::read_file_bounded(impl_->files->absolute(rel), impl_->cfg.max_jpeg_bytes, out, err)) return 3;
    return 0;
}

int VisualMemoryReader::read_thumbnail(int64_t id, int max_w, std::vector<uint8_t>& out, std::string& err) {
    std::vector<uint8_t> full;
    const int rc = read_jpeg(id, full, err);
    if (rc != 0) return rc;
    max_w = std::max(32, std::min(max_w, 1024));
    if (!detail::make_thumbnail(full.data(), full.size(), max_w, 72, out, err)) { err = "thumbnail: " + err; return 3; }
    return 0;
}

std::vector<SearchResult> VisualMemoryReader::search_similar(const std::vector<float>& embedding,
                                                             const SearchOptions& o, SearchStatus* status,
                                                             const EmbeddingSpec* query_space) {
    const int64_t t0 = steady_now_ns();
    SearchStatus st;
    std::vector<SearchResult> res;
    auto done = [&]() { st.elapsed_ms = (steady_now_ns() - t0) / 1e6; if (status) *status = st; return res; };
    if (!impl_->opened) { st = mk(SearchStatus::Code::NotEnabled, "store not open"); return done(); }
    detail::ModelInfo m;
    if (query_space) {
        bool found = false;
        for (auto& mm : impl_->vs->models()) if (same_space(mm.spec, *query_space)) { m = mm; found = true; }
        if (!found) { st = mk(SearchStatus::Code::ModelMismatch, "no stored vectors for this embedding space"); return done(); }
    } else {
        // No declared space: only unambiguous when exactly one model exists.
        auto ms = impl_->vs->models();
        if (ms.size() != 1) { st = mk(SearchStatus::Code::NotEnabled, ms.empty() ? "no embeddings stored" : "multiple models stored: specify the query space"); return done(); }
        m = ms[0];
    }
    res = detail::search_core(impl_->db, *impl_->vs, impl_->files.get(), m, embedding, o, st, query_space);
    return done();
}

std::vector<SearchResult> VisualMemoryReader::search_by_image(const ImageView& image, const SearchOptions& o,
                                                              SearchStatus* status) {
    const int64_t t0 = steady_now_ns();
    auto fail = [&](SearchStatus::Code c, std::string msg) {
        if (status) { *status = mk(c, std::move(msg)); status->elapsed_ms = (steady_now_ns() - t0) / 1e6; }
        return std::vector<SearchResult>{};
    };
    if (!impl_->opened) return fail(SearchStatus::Code::NotEnabled, "store not open");
    if (!impl_->provider || !impl_->provider->ready())
        return fail(SearchStatus::Code::ProviderFailed, "embedding provider not available: " +
                    (impl_->provider ? impl_->provider->last_error() : std::string("none configured")));
    std::vector<float> q;
    if (!impl_->provider->embed(image, q))
        return fail(SearchStatus::Code::ProviderFailed, "inference failed: " + impl_->provider->last_error());
    const EmbeddingSpec ps = impl_->provider->spec();
    SearchStatus st;
    auto r = search_similar(q, o, &st, &ps);
    st.elapsed_ms = (steady_now_ns() - t0) / 1e6;        // include query embedding time
    if (status) *status = st;
    return r;
}

std::vector<SearchResult> VisualMemoryReader::search_by_jpeg(const std::vector<uint8_t>& jpeg, const SearchOptions& o,
                                                             SearchStatus* status) {
    detail::DecodedImage img;
    std::string err;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) {
        if (status) *status = mk(SearchStatus::Code::InvalidArgument, "query jpeg: " + err);
        return {};
    }
    ImageView v; v.data = img.rgb.data(); v.width = img.width; v.height = img.height;
    v.stride = img.width * 3; v.format = PixelFormat::RGB24;
    return search_by_image(v, o, status);
}

std::vector<SearchResult> VisualMemoryReader::search_by_frame_id(int64_t id, const SearchOptions& o,
                                                                 SearchStatus* status, bool* used_stored) {
    const int64_t t0 = steady_now_ns();
    SearchStatus st;
    std::vector<SearchResult> res;
    if (used_stored) *used_stored = false;
    auto done = [&]() { st.elapsed_ms = (steady_now_ns() - t0) / 1e6; if (status) *status = st; return res; };
    if (!impl_->opened) { st = mk(SearchStatus::Code::NotEnabled, "store not open"); return done(); }
    FrameDetail d;
    if (!get_frame(id, d)) { st = mk(SearchStatus::Code::InvalidArgument, "unknown frame id"); return done(); }

    // Prefer the provider's model (so results stay in the space the dashboard advertises), else the
    // frame's own ok vector.
    const auto models = impl_->vs->models();
    detail::ModelInfo pm; const bool have_pm = impl_->model_for_provider(pm);
    detail::ModelInfo chosen; bool have_vec = false;
    std::vector<float> q;
    auto try_model = [&](const detail::ModelInfo& m) {
        for (const auto& e : d.embeddings) if (e.model_id == m.model_id && e.ok)
            return impl_->vs->get_vector(m, id, q);
        return false;
    };
    if (have_pm && try_model(pm)) { chosen = pm; have_vec = true; }
    else for (const auto& m : models) if (!have_vec && try_model(m)) { chosen = m; have_vec = true; }

    if (have_vec) {
        if (used_stored) *used_stored = true;
        res = detail::search_core(impl_->db, *impl_->vs, impl_->files.get(), chosen, q, o, st, nullptr, id);
        return done();
    }
    // No stored vector: embed the JPEG if possible.
    if (!have_pm || !impl_->provider || !impl_->provider->ready()) {
        st = mk(SearchStatus::Code::NotEnabled, "frame has no stored embedding and no matching provider is available");
        return done();
    }
    std::vector<uint8_t> jpeg; std::string err;
    const int rc = read_jpeg(id, jpeg, err);
    if (rc != 0) { st = mk(SearchStatus::Code::InvalidArgument, rc == 2 ? "frame image expired" : "frame image unavailable: " + err); return done(); }
    detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) { st = mk(SearchStatus::Code::InvalidArgument, "decode: " + err); return done(); }
    ImageView v; v.data = img.rgb.data(); v.width = img.width; v.height = img.height; v.stride = img.width * 3; v.format = PixelFormat::RGB24;
    if (!impl_->provider->embed(v, q)) { st = mk(SearchStatus::Code::ProviderFailed, "inference failed: " + impl_->provider->last_error()); return done(); }
    const EmbeddingSpec ps = impl_->provider->spec();
    res = detail::search_core(impl_->db, *impl_->vs, impl_->files.get(), pm, q, o, st, &ps, id);
    return done();
}

} // namespace visualmemory
