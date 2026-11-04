// SPDX-License-Identifier: MIT
#include "visualmemory/visual_memory.h"

#include "embedding_math.h"
#include "embedding_worker.h"
#include "file_store.h"
#include "frame_encoder.h"
#include "retention.h"
#include "sha256.h"
#include "jpeg_decoder.h"
#include "sqlite_store.h"
#include "vector_store.h"
#include "visualmemory/latency_ring.h"

#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <algorithm>
#include <thread>
#include <vector>

namespace visualmemory {

namespace {

int64_t now_utc_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int64_t now_steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t now_steady_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* pixel_format_name(PixelFormat f) {
    switch (f) {
        case PixelFormat::RGB24: return "RGB24";
        case PixelFormat::BGR24: return "BGR24";
        case PixelFormat::NV12:  return "NV12";
        case PixelFormat::JPEG:  return "JPEG";
    }
    return "UNKNOWN";
}

} // namespace

struct VisualMemory::Impl {
    explicit Impl(Config c) : cfg(std::move(c)) {}

    Config cfg;
    detail::SqliteStore db;
    std::unique_ptr<detail::FileStore> files;

    // Ingest queue.
    std::mutex              mu;
    std::condition_variable cv;
    std::deque<OwnedFrame>  queue;
    std::atomic<bool>       stop_flag{false};
    std::atomic<bool>       running{false};
    std::thread             worker;

    // Counters (assembled into Stats on demand).
    std::atomic<uint64_t> samples_seen{0}, accepted{0}, dropped_queue_full{0},
        dropped_invalid{0}, encoded{0}, jpeg_write_failures{0}, database_failures{0},
        retention_deletions{0}, bytes_on_disk{0};
    std::atomic<size_t> max_queue_depth{0};
    LatencyRing lat_encode, lat_write, lat_db, lat_total;

    // Stage 2
    std::shared_ptr<EmbeddingProvider> provider;
    std::unique_ptr<detail::VectorStore> vs;          // always present: retention must honour embeddings
    std::unique_ptr<detail::EmbeddingWorker> ew;
    detail::ModelInfo model;
    std::string embed_status = "disabled";
    std::string vec_backend  = "none";

    void worker_loop();
    void process(OwnedFrame&& f);
};

void VisualMemory::Impl::process(OwnedFrame&& f) {
    const int64_t t0 = now_steady_ns();
    std::string err;
    detail::EncodedJpeg enc;
    if (!detail::encode_frame(f, cfg.stored_width, cfg.stored_height, cfg.jpeg_quality,
                              enc, err)) {
        jpeg_write_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int64_t t1 = now_steady_ns();
    lat_encode.add(t1 - t0);

    const int64_t cap_ms = f.metadata.capture_utc_ms ? f.metadata.capture_utc_ms : now_utc_ms();
    const std::string rel =
        files->make_relative_path(f.metadata.source_id, cap_ms, f.metadata.source_sequence);

    const int64_t t2 = now_steady_ns();
    if (!files->write_atomic(rel, enc.bytes)) {
        jpeg_write_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int64_t t3 = now_steady_ns();
    lat_write.add(t3 - t2);

    detail::InsertRow row;
    row.source_id = f.metadata.source_id;
    row.source_type = f.metadata.source_type;
    row.device_id = f.metadata.device_id;
    row.capture_utc_ms = cap_ms;
    row.pts_ns = f.metadata.pts_ns;
    row.source_sequence = f.metadata.source_sequence;
    row.stored_width = enc.width;
    row.stored_height = enc.height;
    row.pixel_format = pixel_format_name(f.pixel_format);
    row.relative_jpeg_path = rel;
    row.jpeg_size_bytes = enc.bytes.size();
    row.created_utc_ms = now_utc_ms();
    if (cfg.compute_sha256)
        row.jpeg_sha256 = detail::sha256_hex(enc.bytes.data(), enc.bytes.size());

    const int64_t t4 = now_steady_ns();
    const int64_t new_id = db.insert(row);
    if (new_id < 0) {
        database_failures.fetch_add(1, std::memory_order_relaxed);
        files->remove(rel);  // keep disk/db consistent — drop the orphan JPEG
        return;
    }
    const int64_t t5 = now_steady_ns();
    lat_db.add(t5 - t4);
    lat_total.add(t5 - t0);
    encoded.fetch_add(1, std::memory_order_relaxed);
    bytes_on_disk.fetch_add(enc.bytes.size(), std::memory_order_relaxed);
    // Stage 2: hand (id, path) to the separate bounded embedding queue. Non-blocking: a
    // full queue only counts a drop — backfill can pick the frame up later.
    if (ew) ew->try_enqueue(new_id, rel, cap_ms, f.metadata.source_id);
}

void VisualMemory::Impl::worker_loop() {
    int64_t last_retention = now_steady_ms();
    // Initial reconciliation: clear expired/over-quota data from prior runs.
    {
        auto r = detail::run_retention(db, *files, now_utc_ms(), cfg.retention_hours,
                                       cfg.max_storage_bytes, vs.get(), cfg.embedding.retention_days);
        retention_deletions.fetch_add(r.deleted, std::memory_order_relaxed);
        bytes_on_disk.store(db.total_jpeg_bytes(), std::memory_order_relaxed);
        if (ew) ew->refresh_counts();
    }

    for (;;) {
        OwnedFrame frame;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::milliseconds(250), [&] {
                return stop_flag.load(std::memory_order_relaxed) || !queue.empty();
            });
            if (queue.empty()) {
                if (stop_flag.load(std::memory_order_relaxed)) break;
            } else {
                frame = std::move(queue.front());
                queue.pop_front();
            }
        }
        if (frame.width != 0 || frame.pixel_format == PixelFormat::JPEG)
            process(std::move(frame));

        const int64_t now = now_steady_ms();
        if (now - last_retention >= cfg.retention_interval_ms) {
            auto r = detail::run_retention(db, *files, now_utc_ms(), cfg.retention_hours,
                                           cfg.max_storage_bytes, vs.get(), cfg.embedding.retention_days);
            retention_deletions.fetch_add(r.deleted, std::memory_order_relaxed);
            bytes_on_disk.store(db.total_jpeg_bytes(), std::memory_order_relaxed);
            if (ew) ew->refresh_counts();
            last_retention = now;
        }
    }
}

VisualMemory::VisualMemory(Config cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}

VisualMemory::~VisualMemory() { stop(); }

bool VisualMemory::start() {
    if (impl_->running.load()) return true;
    const Config& c = impl_->cfg;
    if (c.root_directory.empty()) return false;
    if (c.stored_width <= 0 || c.stored_height <= 0) return false;
    if (c.max_pending_frames == 0) return false;

    impl_->files = std::make_unique<detail::FileStore>(c.root_directory);
    if (!impl_->files->init()) return false;
    impl_->files->reap_temp_files();

    std::string err;
    const std::string db_path = (std::filesystem::path(c.root_directory) / "visual_memory.db").string();
    if (!impl_->db.open(db_path, err)) return false;
    impl_->vs = std::make_unique<detail::VectorStore>(impl_->db);

    // Stage 2 setup. Any failure here degrades Stage 2 only — Stage 1 keeps running.
    if (c.embedding.enabled) {
        if (!impl_->provider) {
            impl_->embed_status = "no provider";
        } else if (!impl_->provider->ready()) {
            impl_->embed_status = "provider not ready: " + impl_->provider->last_error();
        } else {
            std::string merr;
            if (!impl_->vs->register_model(impl_->provider->spec(), c.embedding.prefer_sqlite_vec,
                                           impl_->model, merr)) {
                impl_->embed_status = "model registration failed: " + merr;
            } else {
                impl_->vec_backend = impl_->model.backend == "vec0"
                    ? "sqlite-vec " + impl_->db.vec_version() : "blob-cosine-fallback";
                impl_->vs->reconcile(impl_->model);
                impl_->ew = std::make_unique<detail::EmbeddingWorker>(
                    impl_->db, *impl_->files, *impl_->vs, impl_->provider, c.embedding, impl_->model,
                    static_cast<int64_t>(c.retention_hours) * 3600 * 1000);
                impl_->embed_status = "active";
            }
        }
    }

    impl_->bytes_on_disk.store(impl_->db.total_jpeg_bytes());
    impl_->stop_flag.store(false);
    impl_->running.store(true);
    if (impl_->ew) impl_->ew->start();
    impl_->worker = std::thread([this] { impl_->worker_loop(); });
    return true;
}

bool VisualMemory::try_submit(OwnedFrame&& frame) {
    if (!impl_->running.load(std::memory_order_relaxed)) return false;
    impl_->samples_seen.fetch_add(1, std::memory_order_relaxed);

    std::string err;
    if (!detail::validate_frame(frame, err)) {
        impl_->dropped_invalid.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->queue.size() >= impl_->cfg.max_pending_frames) {
            impl_->dropped_queue_full.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        impl_->queue.push_back(std::move(frame));
        const size_t depth = impl_->queue.size();  // high-water mark, under the lock we already hold
        if (depth > impl_->max_queue_depth.load(std::memory_order_relaxed))
            impl_->max_queue_depth.store(depth, std::memory_order_relaxed);
    }
    impl_->accepted.fetch_add(1, std::memory_order_relaxed);
    impl_->cv.notify_one();
    return true;
}

std::vector<FrameInfo> VisualMemory::query_by_time(int64_t start_utc_ms, int64_t end_utc_ms,
                                                   const std::string& source_id,
                                                   int limit, int offset) {
    return impl_->db.query_by_time(start_utc_ms, end_utc_ms, source_id, limit, offset);
}

std::string VisualMemory::absolute_path(const std::string& relative_jpeg_path) const {
    if (!impl_->files) return {};
    return impl_->files->absolute(relative_jpeg_path);
}

void VisualMemory::set_embedding_provider(std::shared_ptr<EmbeddingProvider> provider) {
    if (!impl_->running.load()) impl_->provider = std::move(provider);
}

bool VisualMemory::embeddings_active() const {
    return impl_->ew && impl_->running.load() && impl_->provider && impl_->provider->ready();
}

namespace {
SearchStatus fail_status(SearchStatus::Code c, std::string m) {
    SearchStatus s; s.code = c; s.message = std::move(m); return s;
}
}

std::vector<SearchResult> VisualMemory::search_similar(const std::vector<float>& embedding,
                                                       const SearchOptions& o, SearchStatus* status,
                                                       const EmbeddingSpec* query_space) {
    const int64_t t0 = steady_now_ns();
    SearchStatus st;
    std::vector<SearchResult> results;
    auto done = [&]() {
        st.elapsed_ms = (steady_now_ns() - t0) / 1e6;
        if (status) *status = st;
        return results;
    };
    if (!impl_->running.load() || !impl_->ew) {
        st = fail_status(SearchStatus::Code::NotEnabled, "embeddings not active: " + impl_->embed_status);
        return done();
    }
    const detail::ModelInfo& m = impl_->model;
    if (query_space) {
        const EmbeddingSpec& a = *query_space; const EmbeddingSpec& b = m.spec;
        if (a.model_name != b.model_name || a.model_version != b.model_version ||
            a.model_sha256 != b.model_sha256 || a.preprocessing != b.preprocessing ||
            a.dimension != b.dimension || a.dtype != b.dtype || a.metric != b.metric) {
            st = fail_status(SearchStatus::Code::ModelMismatch,
                             "query embedding space " + a.model_name + "/" + a.model_version + "/" +
                             a.preprocessing + " != stored " + b.model_name + "/" + b.model_version + "/" +
                             b.preprocessing);
            return done();
        }
    }
    std::vector<float> q = embedding;
    const detail::VecCheck vc = detail::l2_normalize(q, m.spec.dimension);
    if (vc == detail::VecCheck::BadDimension) {
        st = fail_status(SearchStatus::Code::DimensionMismatch,
                         "query dimension " + std::to_string(embedding.size()) + " != stored " +
                         std::to_string(m.spec.dimension));
        return done();
    }
    if (vc != detail::VecCheck::Ok) {
        st = fail_status(SearchStatus::Code::InvalidVector, std::string("invalid query vector: ") + detail::to_string(vc));
        return done();
    }
    if (o.top_k < 1) { st = fail_status(SearchStatus::Code::InvalidArgument, "top_k must be >= 1"); return done(); }
    if (o.has_min_similarity && !std::isfinite(o.min_similarity)) {
        st = fail_status(SearchStatus::Code::InvalidArgument, "min_similarity not finite");
        return done();
    }
    const int k = std::min(o.top_k, 100);
    const int kq = o.require_image ? std::min(k * 4, 400) : k;   // over-fetch when post-filtering

    std::vector<detail::KnnHit> hits;
    std::string err;
    if (!impl_->vs->knn(m, q.data(), kq, o.start_utc_ms, o.end_utc_ms, o.source_id, hits, &st.candidates_scanned, err)) {
        st = fail_status(SearchStatus::Code::DatabaseError, err);
        return done();
    }
    for (const auto& h : hits) {
        const float sim = 1.0f - h.distance;
        if (o.has_min_similarity && sim < o.min_similarity) continue;
        FrameInfo fi;
        if (!impl_->db.get_by_id(h.frame_id, fi)) continue;     // vector without a frame row: skip
        SearchResult r;
        r.frame_id = fi.id; r.metadata = fi.metadata; r.relative_jpeg_path = fi.relative_jpeg_path;
        r.image_available = fi.image_available;
        r.absolute_path = impl_->files ? impl_->files->absolute(fi.relative_jpeg_path) : std::string{};
        std::error_code ec;
        r.image_exists = fi.image_available && !r.absolute_path.empty() &&
                         std::filesystem::exists(r.absolute_path, ec);
        r.model_id = m.model_id; r.similarity = sim; r.distance = h.distance;
        if (o.require_image && !r.image_exists) continue;
        results.push_back(std::move(r));
        if (static_cast<int>(results.size()) >= k) break;
    }
    std::stable_sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
        return a.similarity != b.similarity ? a.similarity > b.similarity : a.frame_id < b.frame_id; });
    st.code = SearchStatus::Code::Ok;
    impl_->ew->note_search(steady_now_ns() - t0);
    return done();
}

std::vector<SearchResult> VisualMemory::search_by_image(const ImageView& image, const SearchOptions& o,
                                                        SearchStatus* status) {
    if (!impl_->ew) {
        if (status) *status = fail_status(SearchStatus::Code::NotEnabled, "embeddings not active: " + impl_->embed_status);
        return {};
    }
    std::vector<float> q;
    std::string err;
    if (!impl_->ew->embed_image(image, q, err)) {
        if (status) *status = fail_status(SearchStatus::Code::ProviderFailed, err);
        return {};
    }
    return search_similar(q, o, status, &impl_->model.spec);
}

std::vector<SearchResult> VisualMemory::search_by_jpeg(const std::vector<uint8_t>& jpeg,
                                                       const SearchOptions& o, SearchStatus* status) {
    detail::DecodedImage img;
    std::string err;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) {
        if (status) *status = fail_status(SearchStatus::Code::InvalidArgument, "query jpeg: " + err);
        return {};
    }
    ImageView v; v.data = img.rgb.data(); v.width = img.width; v.height = img.height;
    v.stride = img.width * 3; v.format = PixelFormat::RGB24;
    return search_by_image(v, o, status);
}

size_t VisualMemory::backfill_embeddings(size_t max_frames, int64_t deadline_ms) {
    if (!impl_->ew || !impl_->running.load()) return 0;
    return impl_->ew->backfill(max_frames, deadline_ms);
}

EmbeddingStats VisualMemory::embedding_stats() const {
    EmbeddingStats s;
    if (impl_->ew) {
        s = impl_->ew->stats();            // cached atomics only: never blocks on SQLite
        s.backend = impl_->vec_backend;
    }
    s.status = impl_->embed_status;
    return s;
}

Stats VisualMemory::stats() const {
    Stats s;
    s.samples_seen = impl_->samples_seen.load();
    s.accepted = impl_->accepted.load();
    s.dropped_queue_full = impl_->dropped_queue_full.load();
    s.dropped_invalid = impl_->dropped_invalid.load();
    s.encoded = impl_->encoded.load();
    s.jpeg_write_failures = impl_->jpeg_write_failures.load();
    s.database_failures = impl_->database_failures.load();
    s.retention_deletions = impl_->retention_deletions.load();
    s.bytes_on_disk = impl_->bytes_on_disk.load();
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        s.queue_depth = impl_->queue.size();
    }
    s.max_queue_depth = impl_->max_queue_depth.load();
    s.encode = impl_->lat_encode.snapshot();
    s.file_write = impl_->lat_write.snapshot();
    s.db_insert = impl_->lat_db.snapshot();
    s.process_total = impl_->lat_total.snapshot();
    return s;
}

void VisualMemory::stop() {
    if (!impl_->running.exchange(false)) {
        if (impl_->worker.joinable()) impl_->worker.join();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->stop_flag.store(true);
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->ew) impl_->ew->stop();       // after the Stage 1 worker: it may still enqueue
    impl_->db.close();
}

bool VisualMemory::running() const { return impl_->running.load(); }

} // namespace visualmemory
