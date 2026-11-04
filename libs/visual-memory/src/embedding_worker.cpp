// SPDX-License-Identifier: MIT
#include "embedding_worker.h"

#include "embedding_math.h"
#include "jpeg_decoder.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace visualmemory::detail {

namespace {
int64_t now_utc_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}

EmbeddingWorker::EmbeddingWorker(SqliteStore& db, FileStore& files, VectorStore& vs,
                                 std::shared_ptr<EmbeddingProvider> provider, EmbeddingOptions opt,
                                 ModelInfo model, int64_t)
    : db_(db), files_(files), vs_(vs), provider_(std::move(provider)), opt_(opt), model_(std::move(model)) {}

EmbeddingWorker::~EmbeddingWorker() { stop(); }

void EmbeddingWorker::refresh_counts() {
    std::lock_guard<std::mutex> lk(counts_mu_);
    vectors_cached_.store(vs_.count(model_));
    vector_bytes_cached_.store(vs_.total_vector_bytes());
}

void EmbeddingWorker::start() {
    if (running_.exchange(true)) return;
    refresh_counts();
    stop_.store(false);
    th_ = std::thread([this] { loop(); });
}

void EmbeddingWorker::stop() {
    if (!running_.exchange(false)) { if (th_.joinable()) th_.join(); return; }
    { std::lock_guard<std::mutex> lk(mu_); stop_.store(true); }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

bool EmbeddingWorker::try_enqueue(int64_t frame_id, const std::string& rel, int64_t capture_ms,
                                  const std::string& source) {
    if (!running_.load(std::memory_order_relaxed)) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.size() >= opt_.max_pending) { dropped_.fetch_add(1); return false; }
        q_.push_back(Task{frame_id, rel, capture_ms, source, false});
        if (q_.size() > max_depth_.load()) max_depth_.store(q_.size());
    }
    enqueued_.fetch_add(1);
    cv_.notify_one();
    return true;
}

bool EmbeddingWorker::embed_image(const ImageView& img, std::vector<float>& out, std::string& err) {
    if (!provider_ || !provider_->ready()) { err = "embedding provider not ready"; return false; }
    if (!provider_->embed(img, out)) { err = "inference failed: " + provider_->last_error(); return false; }
    const VecCheck c = l2_normalize(out, model_.spec.dimension);
    if (c != VecCheck::Ok) { err = std::string("invalid embedding: ") + to_string(c); return false; }
    return true;
}

EmbeddingWorker::Outcome EmbeddingWorker::process(const Task& t) {
    const int64_t t0 = steady_now_ns();
    // 1. read stored JPEG (bounded)
    std::vector<uint8_t> jpeg;
    std::string err;
    if (!files_.is_safe_relative(t.rel) ||
        !read_file_bounded(files_.absolute(t.rel), opt_.max_jpeg_bytes, jpeg, err)) {
        decode_fail_.fetch_add(1);
        vs_.mark_failed(model_, t.frame_id, "read: " + err);
        perm_fail_.fetch_add(1);
        return Outcome::PermanentFail;
    }
    const int64_t t1 = steady_now_ns();
    lat_read_.add(t1 - t0);
    // 2. decode
    DecodedImage img;
    if (!decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) {
        decode_fail_.fetch_add(1);
        vs_.mark_failed(model_, t.frame_id, "decode: " + err);
        perm_fail_.fetch_add(1);
        return Outcome::PermanentFail;
    }
    const int64_t t2 = steady_now_ns();
    lat_decode_.add(t2 - t1);
    // 3. inference with bounded retries for recoverable provider errors
    ImageView v; v.data = img.rgb.data(); v.width = img.width; v.height = img.height;
    v.stride = img.width * 3; v.format = PixelFormat::RGB24;
    std::vector<float> vec;
    bool ok = false;
    for (int attempt = 1; attempt <= std::max(1, opt_.max_attempts); ++attempt) {
        if (!provider_->ready()) break;
        if (provider_->embed(v, vec)) { ok = true; break; }
        infer_fail_.fetch_add(1);
        if (attempt < opt_.max_attempts) {
            retries_.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(50 * attempt));
        }
    }
    if (!ok) return Outcome::TransientFail;   // recoverable: no DB marker, backfill may retry later
    const VecCheck c = l2_normalize(vec, model_.spec.dimension);
    if (c != VecCheck::Ok) {
        invalid_.fetch_add(1);
        vs_.mark_failed(model_, t.frame_id, std::string("invalid vector: ") + to_string(c));
        perm_fail_.fetch_add(1);
        return Outcome::PermanentFail;
    }
    // 4. persist (one transaction)
    const int64_t t3 = steady_now_ns();
    InsertResult r;
    {
        std::lock_guard<std::mutex> lk(counts_mu_);       // insert + counter update are one unit for refresh_counts()
        r = vs_.insert(model_, t.frame_id, vec.data(), t.capture_ms, t.source, err);
        if (r == InsertResult::Inserted) {
            vectors_cached_.fetch_add(1);
            vector_bytes_cached_.fetch_add(static_cast<uint64_t>(model_.spec.dimension) * sizeof(float));
        }
    }
    const int64_t t4 = steady_now_ns();
    if (r == InsertResult::Error) { db_fail_.fetch_add(1); return Outcome::TransientFail; }
    if (r == InsertResult::AlreadyExists) return Outcome::Exists;
    lat_insert_.add(t4 - t3);
    lat_total_.add(t4 - t0);
    embedded_.fetch_add(1);
    if (t.backfill) backfilled_.fetch_add(1);
    return Outcome::Stored;
}

size_t EmbeddingWorker::backfill(size_t max_frames, int64_t deadline_ms) {
    const int64_t start = steady_now_ns() / 1000000;
    const int64_t min_cap = opt_.retention_days > 0
        ? now_utc_ms() - static_cast<int64_t>(opt_.retention_days) * 86400000LL : 0;
    int64_t before = std::numeric_limits<int64_t>::max();   // cursor: transient failures can't loop
    size_t stored = 0, seen = 0;
    while (seen < max_frames && !stop_.load()) {
        const int want = static_cast<int>(std::min<size_t>(opt_.backfill_batch > 0 ? opt_.backfill_batch : 8,
                                                           max_frames - seen));
        auto batch = vs_.frames_missing(model_, min_cap, before, want);
        if (batch.empty()) break;
        for (const auto& f : batch) {
            ++seen;
            before = std::min(before, f.capture_utc_ms);
            if (process(Task{f.id, f.relative_path, f.capture_utc_ms, f.source_id, true}) == Outcome::Stored)
                ++stored;
            if (deadline_ms > 0 && steady_now_ns() / 1000000 - start >= deadline_ms) return stored;
        }
        // frames sharing the cursor timestamp could be skipped; acceptable: next pass picks them up
        if (opt_.backfill_pause_ms > 0 && seen < max_frames)
            std::this_thread::sleep_for(std::chrono::milliseconds(opt_.backfill_pause_ms));
    }
    return stored;
}

void EmbeddingWorker::loop() {
    int64_t next_backfill = steady_now_ns() / 1000000;   // first cycle immediately if enabled
    for (;;) {
        Task t{};
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(250),
                         [&] { return stop_.load() || !q_.empty(); });
            if (!q_.empty()) { t = std::move(q_.front()); q_.pop_front(); have = true; }
            else if (stop_.load()) break;
        }
        if (have) { process(t); continue; }
        // idle: low-priority backfill, one bounded batch per cycle
        if (opt_.backfill_on_startup && provider_->ready()) {
            const int64_t now = steady_now_ns() / 1000000;
            if (now >= next_backfill) {
                const size_t n = backfill(static_cast<size_t>(std::max(1, opt_.backfill_batch)), 0);
                // nothing left to do => rescan rarely (picks up queue-full drops); else keep going soon
                next_backfill = steady_now_ns() / 1000000 + (n == 0 ? 30000 : opt_.backfill_pause_ms);
            }
        }
    }
}

EmbeddingStats EmbeddingWorker::stats() const {
    EmbeddingStats s;
    s.active = running_.load() && provider_ && provider_->ready();
    s.model_id = model_.model_id;
    s.dimension = model_.spec.dimension;
    s.enqueued = enqueued_.load(); s.dropped_queue_full = dropped_.load();
    s.embedded = embedded_.load(); s.backfilled = backfilled_.load(); s.retries = retries_.load();
    s.inference_failures = infer_fail_.load(); s.invalid_vectors = invalid_.load();
    s.decode_failures = decode_fail_.load(); s.failed_permanent = perm_fail_.load();
    s.db_failures = db_fail_.load(); s.searches = searches_.load();
    s.vectors_stored = vectors_cached_.load(); s.vector_bytes = vector_bytes_cached_.load();
    { std::lock_guard<std::mutex> lk(mu_); s.queue_depth = q_.size(); }
    s.max_queue_depth = max_depth_.load();
    s.file_read = lat_read_.snapshot(); s.jpeg_decode = lat_decode_.snapshot();
    s.vector_insert = lat_insert_.snapshot(); s.process_total = lat_total_.snapshot();
    s.search = lat_search_.snapshot();
    const auto ps = provider_->provider_stats();
    s.preprocess = ps.preprocess; s.inference = ps.inference;
    return s;
}

} // namespace visualmemory::detail
