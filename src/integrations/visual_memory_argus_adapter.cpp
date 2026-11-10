// SPDX-License-Identifier: MIT
#include "integrations/visual_memory_argus_adapter.h"

#include "metrics/log_global.h"
#include "visualmemory/visual_memory.h"
#ifdef ARGUS_VISUAL_EMBEDDINGS
#include "rknn_embedding_provider.h"
#endif

#include <3rdparty/nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace argus {

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
double pct_ms(std::vector<uint32_t> v, double p) {  // v in microseconds, nearest-rank
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(std::ceil(p * v.size()));
    if (idx > 0) --idx;
    return v[std::min(idx, v.size() - 1)] / 1000.0;
}
} // namespace

struct VisualMemoryAdapter::Impl {
    std::unique_ptr<visualmemory::VisualMemory> vm;
    std::shared_ptr<visualmemory::EmbeddingProvider> provider;   // Stage 2 (RKNN); null when off/failed
    int64_t last_sample_steady_ms = 0;
    bool    first = true;
    // Rate-limit the "queue full" warning so a stalled card can't spam the log.
    int64_t last_drop_warn_ms = 0;

    // Benchmark instrumentation (capture-thread only, so no locking). `hook_us` holds the
    // cost of the SAMPLED path (gate + 1 BGR copy + enqueue), one entry per stored sample.
    // The not-sampled path (taken on ~every frame) is only summarised as calls/mean/max.
    std::vector<uint32_t> hook_us;
    uint64_t gate_calls = 0, gate_ns_sum = 0, gate_ns_max = 0;
    int64_t  last_perf_log_ms = 0;

    // Runtime snapshot for the dashboard API (vm_api_server). Written by its OWN thread every 5 s —
    // never from the capture thread — to tmpfs via atomic rename. Reads only lock-free/cached counters.
    std::thread             status_thr;
    std::mutex              status_mu;
    std::condition_variable status_cv;
    bool                    status_stop = false;
};

namespace {
constexpr const char* kStatusFile = "/tmp/argus_vm_status.json";

void write_status_snapshot(visualmemory::VisualMemory& vm,
                           const std::shared_ptr<visualmemory::EmbeddingProvider>& provider) {
    using nlohmann::json;
    const auto s = vm.stats();
    json j;
    j["written_utc_ms"] = now_utc_ms();
    j["stage1"] = {{"samples_seen", s.samples_seen}, {"accepted", s.accepted}, {"encoded", s.encoded},
                   {"dropped_queue_full", s.dropped_queue_full}, {"dropped_invalid", s.dropped_invalid},
                   {"jpeg_write_failures", s.jpeg_write_failures}, {"database_failures", s.database_failures},
                   {"retention_deletions", s.retention_deletions}, {"queue_depth", s.queue_depth},
                   {"max_queue_depth", s.max_queue_depth}, {"bytes_on_disk", s.bytes_on_disk},
                   {"last_commit_utc_ms", s.last_commit_utc_ms}, {"retention_pending", s.retention_pending},
                   {"retention_slices", s.retention_slices}};
    {   // details of the most recent database failure (the counter above is cumulative since the process started)
        const auto& e = s.last_database_error;
        j["stage1"]["last_database_error"] = {{"count", e.count}, {"code", e.code}, {"extended_code", e.extended_code}, {"code_name", e.code_name},
                                              {"message", e.message}, {"operation", e.operation}, {"utc_ms", e.utc_ms}, {"frame_sequence", e.frame_sequence}};
    }
    auto lat = [](const visualmemory::LatencyStats& l) {
        return json{{"count", l.count}, {"p50_ms", l.p50_ms}, {"p95_ms", l.p95_ms}, {"p99_ms", l.p99_ms}, {"max_ms", l.max_ms}};
    };
    j["stage1_latency"] = {{"encode", lat(s.encode)}, {"file_write", lat(s.file_write)}, {"db_insert", lat(s.db_insert)},
                           {"process_total", lat(s.process_total)}, {"retention_slice", lat(s.retention_slice)},
                           {"commit_delay", lat(s.commit_delay)}};
    if (provider) {
        const auto e = vm.embedding_stats();
        j["stage2"] = {{"status", e.status}, {"backend", e.backend}, {"embedded", e.embedded},
                       {"vectors_stored", e.vectors_stored}, {"vector_bytes", e.vector_bytes},
                       {"queue_depth", e.queue_depth}, {"max_queue_depth", e.max_queue_depth},
                       {"dropped_queue_full", e.dropped_queue_full}, {"inference_failures", e.inference_failures},
                       {"invalid_vectors", e.invalid_vectors}, {"decode_failures", e.decode_failures},
                       {"permanent_failures", e.failed_permanent}, {"db_failures", e.db_failures},
                       {"backfilled", e.backfilled}, {"backfill_attempted", e.backfill_attempted},
                       {"backfill_cap", e.backfill_cap}, {"backfill_done", e.backfill_done},
                       {"inference_p50_ms", e.process_total.p50_ms}, {"inference_p95_ms", e.process_total.p95_ms}};
        const auto sp = provider->spec();
        j["model"] = {{"name", sp.model_name}, {"version", sp.model_version}, {"preprocessing", sp.preprocessing},
                      {"dimension", sp.dimension}, {"sha256", sp.model_sha256}, {"dtype", sp.dtype}, {"metric", sp.metric},
                      {"provider_ready", provider->ready()}};
    } else {
        j["stage2"] = nullptr;
        j["model"] = nullptr;
    }
    const std::string tmp = std::string(kStatusFile) + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
        const std::string body = j.dump();
        std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
        std::rename(tmp.c_str(), kStatusFile);
    }
}
} // namespace

VisualMemoryAdapter::VisualMemoryAdapter(VisualMemoryOptions opt)
    : opt_(std::move(opt)), impl_(std::make_unique<Impl>()) {}

VisualMemoryAdapter::~VisualMemoryAdapter() { stop(); }

bool VisualMemoryAdapter::start() {
    if (!opt_.enabled) {
        LG_INFO("visual_memory: disabled (storage OFF by default)");
        return true;
    }
    visualmemory::Config cfg;
    cfg.root_directory     = opt_.storage_root;
    cfg.sample_interval_ms = opt_.sample_interval_ms;
    cfg.stored_width       = opt_.stored_width;
    cfg.stored_height      = opt_.stored_height;
    cfg.jpeg_quality       = opt_.jpeg_quality;
    cfg.max_pending_frames = opt_.max_pending_frames;
    cfg.max_storage_bytes  = opt_.max_storage_bytes;
    cfg.retention_hours    = opt_.retention_hours;

    // ---- Stage 2 (optional). Any failure here degrades Stage 2 only; Stage 1 and Argus continue. ----
    if (opt_.embedding.enabled && !opt_.vector_search_enabled)
        LG_WARN("visual_memory: embedding.enabled=true ignored because vector_search_enabled=false");
    if (opt_.vector_search_enabled && opt_.embedding.enabled) {
#ifdef ARGUS_VISUAL_EMBEDDINGS
        if (opt_.embedding.backend != "rknn") {
            LG_ERROR("visual_memory: unsupported embedding backend '%s' (only 'rknn'); Stage 2 disabled",
                     opt_.embedding.backend.c_str());
        } else {
            visualmemory::RknnEmbeddingProvider::Options ro;
            ro.model_path = opt_.embedding.model_path;
            ro.npu_core   = opt_.embedding.npu_core;
            auto p = std::make_shared<visualmemory::RknnEmbeddingProvider>(ro);
            if (p->init()) {
                impl_->provider = p;
                cfg.embedding.enabled             = true;
                cfg.embedding.max_pending         = opt_.embedding.max_pending;
                cfg.embedding.retention_days      = opt_.embedding.retention_days;
                cfg.embedding.backfill_on_startup = opt_.embedding.backfill_on_startup;
                cfg.embedding.backfill_max_frames = opt_.embedding.backfill_max_frames;
                cfg.embedding.backfill_max_age_hours = opt_.embedding.backfill_max_age_hours;
                cfg.embedding.prefer_sqlite_vec   = opt_.embedding.prefer_sqlite_vec;
                LG_INFO("visual_memory: embedding model loaded: %s (sha256=%.12s) %s",
                        opt_.embedding.model_path.c_str(), p->spec().model_sha256.c_str(), p->describe().c_str());
            } else {
                LG_ERROR("visual_memory: embedding model failed to load (%s); continuing with frame storage only",
                         p->last_error().c_str());
            }
        }
#else
        LG_ERROR("visual_memory: embeddings requested but this build lacks -DENABLE_VISUAL_EMBEDDINGS=ON; ignored");
#endif
    }

    impl_->vm = std::make_unique<visualmemory::VisualMemory>(std::move(cfg));
    if (impl_->provider) impl_->vm->set_embedding_provider(impl_->provider);
    if (!impl_->vm->start()) {
        // Degrade Visual Memory only — Argus inference must keep running.
        LG_ERROR("visual_memory: failed to start at '%s'; continuing without frame storage",
                 opt_.storage_root.c_str());
        impl_->vm.reset();
        if (impl_->provider) { impl_->provider->shutdown(); impl_->provider.reset(); }
        opt_.enabled = false;
        return false;
    }
    if (impl_->provider) {
        const auto es = impl_->vm->embedding_stats();
        if (es.status == "active")
            LG_INFO("visual_memory: stage2 active backend=%s dim=%d retention=%dd queue=%zu",
                    es.backend.c_str(), es.dimension, opt_.embedding.retention_days, opt_.embedding.max_pending);
        else
            LG_WARN("visual_memory: stage2 inactive: %s (frame storage continues)", es.status.c_str());
    }
    impl_->status_thr = std::thread([this] {
        std::unique_lock<std::mutex> lk(impl_->status_mu);
        while (!impl_->status_stop) {
            lk.unlock();
            if (impl_->vm) write_status_snapshot(*impl_->vm, impl_->provider);
            lk.lock();
            impl_->status_cv.wait_for(lk, std::chrono::seconds(5), [this] { return impl_->status_stop; });
        }
    });
    LG_INFO("visual_memory: enabled root='%s' interval=%lldms %dx%d q=%d retention=%dh",
            opt_.storage_root.c_str(), (long long)opt_.sample_interval_ms,
            opt_.stored_width, opt_.stored_height, opt_.jpeg_quality, opt_.retention_hours);
    return true;
}

void VisualMemoryAdapter::stop() {
    if (impl_ && impl_->status_thr.joinable()) {
        { std::lock_guard<std::mutex> lk(impl_->status_mu); impl_->status_stop = true; }
        impl_->status_cv.notify_all();
        impl_->status_thr.join();
    }
    if (impl_ && impl_->vm) {
        impl_->vm->stop();
        impl_->vm.reset();
    }
    if (impl_ && impl_->provider) { impl_->provider->shutdown(); impl_->provider.reset(); }
}

void VisualMemoryAdapter::offer_bgr(const uint8_t* bgr, int width, int height,
                                    int64_t pts_ns, uint64_t seq) noexcept {
    if (!opt_.enabled || !impl_ || !impl_->vm || bgr == nullptr || width <= 0 || height <= 0)
        return;
    const int64_t hook_t0 = now_steady_ns();

    // Sampling gate — steady_clock so accelerated file playback still samples at
    // real wall-clock cadence. First frame is always taken.
    const int64_t now = now_steady_ms();
    if (!impl_->first &&
        (now - impl_->last_sample_steady_ms) < opt_.sample_interval_ms) {
        const uint64_t d = static_cast<uint64_t>(now_steady_ns() - hook_t0);
        ++impl_->gate_calls; impl_->gate_ns_sum += d;
        if (d > impl_->gate_ns_max) impl_->gate_ns_max = d;
        return;
    }
    impl_->first = false;
    impl_->last_sample_steady_ms = now;

    // One bounded copy of the full-resolution source scene.
    visualmemory::OwnedFrame f;
    f.width = width;
    f.height = height;
    f.pixel_format = visualmemory::PixelFormat::BGR24;
    f.stride0 = width * 3;
    const size_t n = static_cast<size_t>(width) * height * 3;
    try {
        f.pixels.resize(n);
    } catch (...) {
        return;  // low memory: skip this sample, never crash the capture thread
    }
    std::memcpy(f.pixels.data(), bgr, n);

    f.metadata.source_id       = opt_.source_id;
    f.metadata.source_type     = opt_.source_type;
    f.metadata.device_id       = opt_.device_id;
    f.metadata.capture_utc_ms  = now_utc_ms();
    f.metadata.pts_ns          = pts_ns;
    f.metadata.source_sequence = seq;

    if (!impl_->vm->try_submit(std::move(f))) {
        const int64_t wall = now_utc_ms();
        if (wall - impl_->last_drop_warn_ms > 30'000) {
            impl_->last_drop_warn_ms = wall;
            auto s = impl_->vm->stats();
            LG_WARN("visual_memory: sample dropped (queue full); encoded=%llu dropped=%llu",
                    (unsigned long long)s.encoded,
                    (unsigned long long)s.dropped_queue_full);
        }
    }

    // Record the sampled-path cost, then emit one summary line per minute.
    {
        const int64_t d_ns = now_steady_ns() - hook_t0;
        if (impl_->hook_us.size() < 4096)
            impl_->hook_us.push_back(static_cast<uint32_t>(std::min<int64_t>(d_ns / 1000, 0xFFFFFFFFLL)));
        if (now - impl_->last_perf_log_ms >= 60'000) {
            impl_->last_perf_log_ms = now;
            const auto s = impl_->vm->stats();
            const uint32_t hmax = impl_->hook_us.empty() ? 0
                : *std::max_element(impl_->hook_us.begin(), impl_->hook_us.end());
            char perf[1400];
            std::snprintf(perf, sizeof(perf), "vm_perf: hook_sample_ms p50=%.3f p95=%.3f p99=%.3f max=%.3f n=%zu | "
                    "gate_us mean=%.2f max=%.2f calls=%llu | "
                    "encode_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f | "
                    "write_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f | "
                    "db_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f | "
                    "proc_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f | "
                    "q_max=%zu q_now=%zu seen=%llu accepted=%llu enc=%llu drop_full=%llu drop_invalid=%llu "
                    "write_fail=%llu db_fail=%llu",
                    pct_ms(impl_->hook_us, .50), pct_ms(impl_->hook_us, .95),
                    pct_ms(impl_->hook_us, .99), hmax / 1000.0, impl_->hook_us.size(),
                    impl_->gate_calls ? impl_->gate_ns_sum / 1000.0 / impl_->gate_calls : 0.0,
                    impl_->gate_ns_max / 1000.0, (unsigned long long)impl_->gate_calls,
                    s.encode.p50_ms, s.encode.p95_ms, s.encode.p99_ms, s.encode.max_ms,
                    s.file_write.p50_ms, s.file_write.p95_ms, s.file_write.p99_ms, s.file_write.max_ms,
                    s.db_insert.p50_ms, s.db_insert.p95_ms, s.db_insert.p99_ms, s.db_insert.max_ms,
                    s.process_total.p50_ms, s.process_total.p95_ms, s.process_total.p99_ms,
                    s.process_total.max_ms,
                    s.max_queue_depth, s.queue_depth,
                    (unsigned long long)s.samples_seen, (unsigned long long)s.accepted,
                    (unsigned long long)s.encoded, (unsigned long long)s.dropped_queue_full,
                    (unsigned long long)s.dropped_invalid,
                    (unsigned long long)s.jpeg_write_failures, (unsigned long long)s.database_failures);
            // The app log level defaults to "warn", so also append to a dedicated file that the benchmark
            // reads. /tmp is tmpfs (RAM): one ~700 B line per minute, no SD-card writes.
            LG_INFO("%s", perf);
            if (FILE* pf = std::fopen("/tmp/vm_perf.log", "a")) { std::fprintf(pf, "%s\n", perf); std::fclose(pf); }
            if (impl_->provider) {   // Stage 2 line (same file, same cadence)
                const auto e = impl_->vm->embedding_stats();
                char ep[1100];
                std::snprintf(ep, sizeof(ep),
                    "vm_emb: status=%s backend=%s dim=%d | preprocess_ms p50=%.2f p95=%.2f p99=%.2f | "
                    "infer_ms p50=%.2f p95=%.2f p99=%.2f | decode_ms p50=%.2f p95=%.2f | "
                    "vec_insert_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f | total_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f | "
                    "q_max=%zu q_now=%zu enq=%llu drop_full=%llu embedded=%llu infer_fail=%llu invalid=%llu "
                    "decode_fail=%llu perm_fail=%llu db_fail=%llu vectors=%llu vec_bytes=%llu",
                    e.status.c_str(), e.backend.c_str(), e.dimension,
                    e.preprocess.p50_ms, e.preprocess.p95_ms, e.preprocess.p99_ms,
                    e.inference.p50_ms, e.inference.p95_ms, e.inference.p99_ms,
                    e.jpeg_decode.p50_ms, e.jpeg_decode.p95_ms,
                    e.vector_insert.p50_ms, e.vector_insert.p95_ms, e.vector_insert.p99_ms, e.vector_insert.max_ms,
                    e.process_total.p50_ms, e.process_total.p95_ms, e.process_total.p99_ms, e.process_total.max_ms,
                    e.max_queue_depth, e.queue_depth, (unsigned long long)e.enqueued,
                    (unsigned long long)e.dropped_queue_full, (unsigned long long)e.embedded,
                    (unsigned long long)e.inference_failures, (unsigned long long)e.invalid_vectors,
                    (unsigned long long)e.decode_failures, (unsigned long long)e.failed_permanent,
                    (unsigned long long)e.db_failures, (unsigned long long)e.vectors_stored,
                    (unsigned long long)e.vector_bytes);
                LG_INFO("%s", ep);
                if (FILE* pf = std::fopen("/tmp/vm_perf.log", "a")) { std::fprintf(pf, "%s\n", ep); std::fclose(pf); }
            }
        }
    }
}

} // namespace argus
