// SPDX-License-Identifier: MIT
#include "visualmemory/object_indexer.h"

#include "jpeg_decoder.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <set>

namespace visualmemory {

namespace {
using Clock = std::chrono::steady_clock;
int64_t utc_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
double ema(double prev, double v) { return prev <= 0 ? v : prev * 0.8 + v * 0.2; }

double iou(const DetectedObject& a, const DetectedObject& b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    const double inter = std::max(0, x1 - x0) * static_cast<double>(std::max(0, y1 - y0));
    const double u = static_cast<double>(a.w) * a.h + static_cast<double>(b.w) * b.h - inter;
    return u > 0 ? inter / u : 0.0;
}
} // namespace

struct ObjectIndexer::Impl {
    ObjectIndexerConfig cfg;
    ObjectIndex& index;
    ProviderFn provider_fn;
    DetectorFn detector_fn;

    // one frame stream (camera or display): its own queue, rate limit, cursor and counters
    struct Src {
        IndexerSource conf;
        std::string type;
        struct Job { std::string kind; int64_t id; };
        std::deque<Job> q;                                  // bounded live queue
        int64_t cursor = -1, enq_cursor = -1;               // persisted live cursor / newest capture time already queued
        Clock::time_point next_due = Clock::time_point{};   // earliest instant of the next live job
        Clock::time_point last_open_try = Clock::time_point{};
        IndexerSourceStats st;
    };
    struct BfJob { std::string source, kind; int64_t id; bool retry; };

    mutable std::mutex mu;                       // queues, flags, stats
    std::condition_variable cv;
    std::vector<std::unique_ptr<Src>> srcs;
    std::deque<BfJob> bq;                        // bounded backfill queue
    bool started = false, stopping = false;
    std::thread th;
    ObjectIndexerStats st;

    // backfill bookkeeping (guarded by mu)
    bool bf_pending = false, bf_active = false, bf_cancel = false;
    BackfillRequest bf_req;
    std::vector<BfJob> bf_ids; size_t bf_next = 0;

    // worker-thread only
    std::shared_ptr<ObjectDetector> det;
    std::shared_ptr<EmbeddingProvider> prov;
    std::string det_name, det_version;
    size_t rr = 0;                               // round-robin position over the sources
    Clock::time_point last_work = Clock::now();
    std::map<std::pair<std::string, int64_t>, int> fail_attempts;   // per-frame detector failures across backoffs

    Impl(ObjectIndexerConfig c, ObjectIndex& i, std::vector<IndexerSource> sources, ProviderFn p, DetectorFn d)
        : cfg(std::move(c)), index(i), provider_fn(std::move(p)), detector_fn(std::move(d)) {
        cfg.frames_per_minute = std::clamp(cfg.frames_per_minute, 1, 600);
        cfg.queue_depth = std::clamp(cfg.queue_depth, 1, 64);
        cfg.max_objects_per_frame = std::clamp(cfg.max_objects_per_frame, 1, 64);
        cfg.max_backfill_frames = std::clamp(cfg.max_backfill_frames, 1, 500);
        cfg.poll_ms = std::clamp(cfg.poll_ms, 50, 60000);
        cfg.min_box_px = std::clamp(cfg.min_box_px, 8, 512);
        for (auto& sc : sources) {
            if (!sc.source) continue;
            auto s = std::make_unique<Src>();
            s->conf = sc; s->conf.frames_per_minute = std::clamp(sc.frames_per_minute, 1, 600); s->conf.queue_depth = std::clamp(sc.queue_depth, 1, 64);
            s->type = sc.source->type();
            s->st.type = s->type; s->st.live_scan = sc.live_scan; s->st.frames_per_minute = s->conf.frames_per_minute; s->st.queue_capacity = s->conf.queue_depth;
            s->st.store_state = "not opened yet";
            srcs.push_back(std::move(s));
        }
        if (!srcs.empty()) { st.queue_capacity = srcs.front()->conf.queue_depth; st.live_scan = srcs.front()->conf.live_scan; }
    }

    Src* find(const std::string& type) { for (auto& s : srcs) if (s->type == type) return s.get(); return nullptr; }

    void set_state(const std::string& s) { std::lock_guard<std::mutex> lk(mu); st.state = s; }
    void fail(const std::string& what, uint64_t ObjectIndexerStats::*counter) {
        std::lock_guard<std::mutex> lk(mu);
        st.last_error = what; st.last_error_utc_ms = utc_now_ms();
        if (counter) ++(st.*counter);
    }
    bool wait_ms(int ms) {                       // interruptible sleep; false when stopping
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::milliseconds(std::max(1, ms)), [&] { return stopping; });
        return !stopping;
    }
    bool is_stopping() { std::lock_guard<std::mutex> lk(mu); return stopping; }
    static std::string cursor_key(const Src& s) { return s.type == "camera" ? "auto_cursor_utc_ms" : "auto_cursor_utc_ms:" + s.type; }

    // Models are loaded lazily, on this thread, the first time a frame needs them.
    bool ensure_models() {
        if (!det || !det->ready()) {
            std::string why;
            det = detector_fn ? detector_fn(why) : nullptr;
            if (!det || !det->ready()) {
                fail("detector unavailable: " + (det ? det->last_error() : why), &ObjectIndexerStats::detector_errors);
                det.reset(); return false;
            }
            det_name = cfg.detector_tag.empty() ? det->name() : cfg.detector_tag; det_version = det->version();
            std::lock_guard<std::mutex> lk(mu); st.detector = det_name; st.detector_version = det_version;
        }
        {   // asked for on every frame: the owner may unload an idle model and reload it here (and tracks its use)
            std::string why;
            prov = provider_fn ? provider_fn(why) : nullptr;
            if (!prov || !prov->ready()) { fail("embedding provider unavailable: " + why, &ObjectIndexerStats::provider_errors); prov.reset(); return false; }
        }
        return true;
    }

    // A frame store may not exist yet (the camera store is written by Argus, the Proof-of-Play store starts with its service): open lazily, at most every 2 s.
    bool source_ok(Src& s) {
        std::string e;
        if (s.conf.source->ensure_open(e)) { std::lock_guard<std::mutex> lk(mu); s.st.store_state = "open"; return true; }
        if (s.last_open_try != Clock::time_point{} && ms_since(s.last_open_try) < 2000) return false;
        s.last_open_try = Clock::now();
        { std::lock_guard<std::mutex> lk(mu); s.st.store_state = e; }
        fail(s.type + " frame store unavailable: " + e, nullptr);
        return false;
    }

    void init_cursor(Src& s) {
        if (s.cursor >= 0) return;
        std::string v;
        if (index.get_meta(cursor_key(s), v)) { try { s.cursor = std::stoll(v); } catch (...) { s.cursor = -1; } }
        if (s.cursor < 0) { s.cursor = utc_now_ms(); index.set_meta(cursor_key(s), std::to_string(s.cursor)); }   // nothing older is touched automatically
        s.enq_cursor = s.cursor;
        std::lock_guard<std::mutex> lk(mu); s.st.cursor_utc_ms = s.cursor;
        if (s.type == "camera") st.cursor_utc_ms = s.cursor;
    }

    // ---- queue feeding -----------------------------------------------------------------------------------------
    void start_backfill_if_pending() {
        // bf_pending stays true while the id list is built, so a second request cannot slip in (request_backfill refuses
        // while pending or active).
        BackfillRequest r;
        { std::lock_guard<std::mutex> lk(mu); if (!bf_pending || bf_active) return; r = bf_req; }
        auto finish_err = [&](const std::string& why) {
            std::lock_guard<std::mutex> lk(mu);
            bf_pending = false; st.backfill_state = "error"; st.backfill_finished_utc_ms = utc_now_ms();
            st.last_error = why; st.last_error_utc_ms = utc_now_ms();
        };
        Src* s = find(r.source);
        if (!s) { finish_err("backfill: no such source '" + r.source + "'"); return; }
        if (!source_ok(*s)) { finish_err("backfill: the " + r.source + " frame store is not available"); return; }
        if (!ensure_models()) { finish_err("backfill: models unavailable (" + st.last_error + ")"); return; }       // the progress key needs the detector identity
        std::vector<BfJob> ids;
        const std::string disp_kind = r.kind == "checkpoint" ? "checkpoint" : "history";
        if (!r.frame_ids.empty()) {
            for (int64_t id : r.frame_ids) if (id > 0 && static_cast<int>(ids.size()) < r.limit) ids.push_back({r.source, r.source == "display" ? disp_kind : std::string(), id, r.retry_failed});
        } else {
            // Only frames whose JPEG is still retained can be searched for objects. Scan a bounded window and keep
            // the first `limit` such frames that this detector has not processed yet.
            const int window = std::min(r.limit * 20 + 200, 5000);
            for (const auto& f : s->conf.source->list_range(r.start_utc_ms, r.end_utc_ms, window, r.newest_first)) {
                if (static_cast<int>(ids.size()) >= r.limit) break;
                if (!f.image_available || !f.image_exists) continue;
                FrameProgress p;
                if (index.get_frame_progress(f.source_type, f.kind, f.id, det_name, det_version, p) && (p.status == "ok" || !r.retry_failed)) continue;
                ids.push_back({r.source, f.kind, f.id, r.retry_failed});
            }
        }
        std::lock_guard<std::mutex> lk(mu);
        if (!bf_pending) return;                                 // cancelled while the list was being built
        bf_pending = false;
        bf_ids = std::move(ids); bf_next = 0; bf_active = true; bf_cancel = false;
        st.backfill_source = r.source;
        st.backfill_state = bf_ids.empty() ? "done" : "running";
        st.backfill_total = static_cast<int>(bf_ids.size()); st.backfill_done = st.backfill_indexed = st.backfill_failed = 0;
        st.backfill_started_utc_ms = utc_now_ms(); st.backfill_finished_utc_ms = bf_ids.empty() ? utc_now_ms() : 0;
        if (bf_ids.empty()) bf_active = false;
    }

    void refill_backfill() {
        std::lock_guard<std::mutex> lk(mu);
        if (!bf_active) return;
        if (bf_cancel) { bf_next = bf_ids.size(); bq.clear(); }
        while (bf_next < bf_ids.size() && static_cast<int>(bq.size()) < std::max(1, cfg.queue_depth)) bq.push_back(bf_ids[bf_next++]);
        if (bf_next >= bf_ids.size() && bq.empty()) {              // all done (the last job already finished)
            bf_active = false; st.backfill_state = bf_cancel ? "cancelled" : "done"; st.backfill_finished_utc_ms = utc_now_ms();
        }
    }

    void refill_live(Src& s) {
        if (!s.conf.live_scan) return;
        { std::lock_guard<std::mutex> lk(mu); if (!s.q.empty()) return; }
        if (!source_ok(s)) return;
        init_cursor(s);
        uint64_t total = 0;
        const auto recs = s.conf.source->list_after(s.enq_cursor, s.conf.queue_depth, &total);   // newest first
        std::lock_guard<std::mutex> lk(mu);
        if (recs.empty()) { s.st.queue_depth = 0; return; }
        if (total > recs.size()) { s.st.skipped_stale += total - recs.size(); st.skipped_stale += total - recs.size(); }   // behind: sampled out (the newest are taken)
        for (auto it = recs.rbegin(); it != recs.rend(); ++it) {
            if (static_cast<int>(s.q.size()) >= s.conf.queue_depth) { ++s.st.dropped_queue_full; ++st.dropped_queue_full; continue; }
            s.q.push_back({it->kind, it->id});
            s.enq_cursor = std::max<int64_t>(s.enq_cursor, it->capture_utc_ms);
        }
        s.st.queue_depth = static_cast<int>(s.q.size());
    }

    // ---- one frame ---------------------------------------------------------------------------------------------
    enum class Outcome { Done, Backoff };

    Outcome process(Src& s, const std::string& kind, int64_t id, bool backfill, bool retry) {
        const auto t0 = Clock::now();
        ObjectFrameSource& src = *s.conf.source;
        FrameProgress prog; prog.source_type = s.type; prog.source_kind = kind; prog.frame_id = id; prog.detector = det_name; prog.detector_version = det_version;
        prog.threshold = cfg.min_confidence; prog.processed_utc_ms = utc_now_ms();
        auto bf_done = [&](bool failed) { if (backfill) { std::lock_guard<std::mutex> lk(mu); ++st.backfill_done; if (failed) ++st.backfill_failed; } };
        FrameProgress old;
        if (index.get_frame_progress(s.type, kind, id, det_name, det_version, old) && (old.status == "ok" || !retry)) { bf_done(false); return Outcome::Done; }   // already handled

        ObjSrcFrame sf;
        if (!src.get(kind, id, sf)) {
            prog.status = "error"; prog.error = "unknown frame"; index.record_frame_progress(prog);
            { std::lock_guard<std::mutex> lk(mu); ++st.frames_failed; ++s.st.frames_failed; } bf_done(true); return Outcome::Done;
        }
        prog.source_id = sf.source_id; prog.capture_utc_ms = sf.capture_utc_ms;
        auto advance_cursor = [&] {
            if (backfill) return;
            s.cursor = std::max(s.cursor, prog.capture_utc_ms); index.set_meta(cursor_key(s), std::to_string(s.cursor));
            std::lock_guard<std::mutex> lk(mu); s.st.cursor_utc_ms = s.cursor; if (s.type == "camera") st.cursor_utc_ms = s.cursor;
        };

        std::vector<uint8_t> jpeg; std::string err;
        const int rc = sf.image_available ? src.read_jpeg(kind, id, jpeg, err) : 2;
        if (rc != 0) {
            prog.status = "image_unavailable"; prog.error = rc == 2 ? "frame image has expired" : "frame image missing";
            index.record_frame_progress(prog);
            { std::lock_guard<std::mutex> lk(mu); ++st.frames_image_unavailable; ++s.st.frames_image_unavailable; }
            bf_done(true); advance_cursor(); return Outcome::Done;
        }
        detail::DecodedImage img;
        if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) {
            prog.status = "error"; prog.error = "decode failed: " + err; index.record_frame_progress(prog);
            { std::lock_guard<std::mutex> lk(mu); ++st.frames_failed; ++s.st.frames_failed; } bf_done(true); advance_cursor(); return Outcome::Done;
        }
        if (!ensure_models()) return Outcome::Backoff;

        ImageView v; v.data = img.rgb.data(); v.width = img.width; v.height = img.height; v.stride = img.width * 3; v.format = PixelFormat::RGB24;
        std::vector<DetectedObject> dets;
        const auto td = Clock::now();
        const auto fkey = std::make_pair(s.type + ":" + kind, id);
        if (!det->detect(v, cfg.min_confidence, dets)) {
            const std::string why = det->last_error();
            fail("detector failed: " + why, &ObjectIndexerStats::detector_errors);
            // A failing detector must not poison frames: back off and retry. Only a frame that fails on its own,
            // repeatedly (3 attempts), is recorded as an error so one bad image cannot stall the worker forever.
            if (fail_attempts.size() > 256) fail_attempts.clear();
            if (++fail_attempts[fkey] < 3) return Outcome::Backoff;
            fail_attempts.erase(fkey);
            prog.status = "error"; prog.error = "detector failed after 3 attempts: " + why; index.record_frame_progress(prog);
            { std::lock_guard<std::mutex> lk(mu); ++st.frames_failed; ++s.st.frames_failed; } bf_done(true); advance_cursor();
            return Outcome::Done;
        }
        fail_attempts.erase(fkey);
        const double detect_ms = det->last_latency_ms() > 0 ? det->last_latency_ms() : ms_since(td);

        // allowlist, size, per-frame cap, duplicate suppression between classes
        std::set<int> allow(cfg.class_ids.begin(), cfg.class_ids.end());
        std::vector<DetectedObject> keep;
        std::sort(dets.begin(), dets.end(), [](const DetectedObject& a, const DetectedObject& b) { return a.confidence > b.confidence; });
        for (auto d : dets) {
            if (!allow.count(d.class_id) || d.confidence < cfg.min_confidence) continue;
            const int x0 = std::clamp(d.x, 0, img.width - 1), y0 = std::clamp(d.y, 0, img.height - 1);
            const int x1 = std::clamp(d.x + d.w, x0 + 1, img.width), y1 = std::clamp(d.y + d.h, y0 + 1, img.height);
            d.x = x0; d.y = y0; d.w = x1 - x0; d.h = y1 - y0;
            if (std::min(d.w, d.h) < cfg.min_box_px) continue;
            bool dup = false;
            for (const auto& k : keep) if (iou(d, k) > cfg.max_duplicate_iou) { dup = true; break; }
            if (dup) continue;
            keep.push_back(d);
            if (static_cast<int>(keep.size()) >= cfg.max_objects_per_frame) break;
        }
        prog.objects_found = static_cast<int>(keep.size());

        // one short transaction for the frame: its objects, the progress record and the live cursor
        std::vector<ObjectIndex::FrameRegion> regs;
        for (const auto& d : keep) {
            DetectionInfo di; di.class_id = d.class_id; di.class_name = d.class_name.empty() ? coco80_name(d.class_id) : d.class_name;
            di.confidence = d.confidence; di.detector = det_name; di.detector_version = det_version;
            regs.push_back({Roi{d.x, d.y, d.w, d.h}, di});
        }
        const int64_t new_cursor = backfill ? -1 : std::max(s.cursor, prog.capture_utc_ms);
        const ObjectIndex::FrameResult fr = index.index_frame(src, *prov, kind, id, regs, prog, backfill ? std::string() : cursor_key(s),
                                                              backfill ? std::string() : std::to_string(new_cursor));
        using C = ObjectIndexResult::Code;
        if (fr.code == C::ProviderFailed || fr.code == C::InvalidVector) { fail("embedding failed: " + fr.message, &ObjectIndexerStats::provider_errors); return Outcome::Backoff; }
        if (fr.code == C::DatabaseError) { fail("object index write failed: " + fr.message, &ObjectIndexerStats::db_errors); return Outcome::Backoff; }
        if (fr.code != C::Indexed) {                              // the frame expired / vanished between listing and indexing
            prog.status = fr.code == C::ImageUnavailable ? "image_unavailable" : "error"; prog.error = fr.message;
            index.record_frame_progress(prog);
            { std::lock_guard<std::mutex> lk(mu);
              if (fr.code == C::ImageUnavailable) { ++st.frames_image_unavailable; ++s.st.frames_image_unavailable; } else { ++st.frames_failed; ++s.st.frames_failed; } }
            bf_done(true); advance_cursor(); return Outcome::Done;
        }
        if (!backfill) { s.cursor = new_cursor; std::lock_guard<std::mutex> lk(mu); s.st.cursor_utc_ms = s.cursor; if (s.type == "camera") st.cursor_utc_ms = s.cursor; }
        const int indexed = fr.indexed, dupes = fr.duplicates; const double idx_ms = fr.elapsed_ms;
        const double total_ms = ms_since(t0);
        std::lock_guard<std::mutex> lk(mu);
        ++st.frames_processed; ++s.st.frames_processed;
        if (indexed + dupes > 0) { ++st.frames_with_objects; ++s.st.frames_with_objects; }
        st.objects_indexed += static_cast<uint64_t>(indexed); s.st.objects_indexed += static_cast<uint64_t>(indexed); st.objects_duplicate += static_cast<uint64_t>(dupes);
        st.last_detect_ms = detect_ms; st.avg_detect_ms = ema(st.avg_detect_ms, detect_ms);
        if (!keep.empty()) { st.last_index_ms = idx_ms; st.avg_index_ms = ema(st.avg_index_ms, idx_ms); s.st.avg_index_ms = ema(s.st.avg_index_ms, idx_ms); }
        if (fr.embed_count > 0) { const double per = fr.embed_ms / fr.embed_count; st.last_embed_ms = per; st.avg_embed_ms = ema(st.avg_embed_ms, per); s.st.avg_embed_ms = ema(s.st.avg_embed_ms, per); }
        s.st.avg_detect_ms = ema(s.st.avg_detect_ms, detect_ms); s.st.avg_frame_total_ms = ema(s.st.avg_frame_total_ms, total_ms);
        st.last_frame_total_ms = total_ms;
        if (backfill) { ++st.backfill_done; st.backfill_indexed += indexed; }
        return Outcome::Done;
    }

    // ---- thread ------------------------------------------------------------------------------------------------
    void run() {
        set_state("starting");
        while (!is_stopping()) {
            start_backfill_if_pending();
            refill_backfill();
            for (auto& s : srcs) refill_live(*s);

            // 1. a live job whose source is due (sources in turn, each at its own sustained rate)
            Src* pick = nullptr; Src::Job lj{};
            const auto now = Clock::now();
            Clock::time_point soonest = Clock::time_point::max();
            {
                std::unique_lock<std::mutex> lk(mu);
                for (size_t k = 0; k < srcs.size(); ++k) {
                    Src& s = *srcs[(rr + k) % srcs.size()];
                    if (s.q.empty()) continue;
                    if (s.next_due <= now) { pick = &s; rr = (rr + k + 1) % srcs.size(); break; }
                    soonest = std::min(soonest, s.next_due);
                }
                if (pick) { lj = pick->q.front(); pick->q.pop_front(); pick->st.queue_depth = static_cast<int>(pick->q.size()); st.state = "running"; }
                size_t queued = 0; for (auto& s : srcs) queued += s->q.size(); st.queue_depth = static_cast<int>(queued);
            }
            if (pick) {
                pick->next_due = Clock::now() + std::chrono::milliseconds(60000 / pick->conf.frames_per_minute);
                const Outcome o = process(*pick, lj.kind, lj.id, false, false);
                last_work = Clock::now();
                if (o == Outcome::Backoff) backoff(pick, nullptr);
                continue;
            }
            // 2. no live job is due: spend the time on one backfill frame (live is re-checked after every frame)
            BfJob bj{}; bool have_bf = false;
            { std::lock_guard<std::mutex> lk(mu); if (!bq.empty()) { bj = bq.front(); bq.pop_front(); have_bf = true; st.state = "running"; } }
            if (have_bf) {
                Src* s = find(bj.source);
                Outcome o = Outcome::Done;
                if (s) o = process(*s, bj.kind, bj.id, true, bj.retry);
                last_work = Clock::now();
                if (o == Outcome::Backoff) { backoff(nullptr, &bj); continue; }
                if (cfg.backfill_pause_ms > 0 && !wait_ms(cfg.backfill_pause_ms)) break;
                continue;
            }
            // 3. idle until the next live job is due, a request arrives or the poll interval elapses
            {
                std::unique_lock<std::mutex> lk(mu);
                st.state = "idle";
                auto until = Clock::now() + std::chrono::milliseconds(cfg.poll_ms);
                if (soonest != Clock::time_point::max()) until = std::min(until, soonest);
                cv.wait_until(lk, until, [&] { return stopping || bf_pending; });
                bool any_live = false; for (auto& s : srcs) any_live = any_live || s->conf.live_scan;
                if (!any_live && !bf_pending && !bf_active && det && ms_since(last_work) > 60000) {   // backfill-only: free the NPU contexts when idle
                    lk.unlock(); det.reset(); prov.reset(); lk.lock();
                }
            }
        }
        det.reset(); prov.reset();
        set_state("stopped");
    }

    // After a model / database failure: the frame stays unprocessed and is retried after the back-off.
    void backoff(Src* live, const BfJob* bf) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (bf && bf_next >= bq.size() + 1) bf_next -= bq.size() + 1;       // re-offer the failed job and the queued ones
            if (bf) bq.clear();
            if (live) { live->q.clear(); live->st.queue_depth = 0; live->enq_cursor = live->cursor; }        // live frames are re-offered after the back-off
            st.state = "backoff";
        }
        wait_ms(cfg.backoff_ms);
    }
};

ObjectIndexer::ObjectIndexer(ObjectIndexerConfig cfg, ObjectIndex& index, std::vector<IndexerSource> sources, ProviderFn p, DetectorFn d)
    : cfg_(cfg), impl_(std::make_unique<Impl>(std::move(cfg), index, std::move(sources), std::move(p), std::move(d))) { cfg_ = impl_->cfg; }

ObjectIndexer::ObjectIndexer(ObjectIndexerConfig cfg, ObjectIndex& index, VisualMemoryReader& reader, ProviderFn p, DetectorFn d)
    : cfg_(cfg), owned_camera_(std::make_unique<CameraObjectSource>(reader)),
      impl_(std::make_unique<Impl>(cfg, index, std::vector<IndexerSource>{IndexerSource{owned_camera_.get(), cfg.live_scan, cfg.frames_per_minute, cfg.queue_depth}}, std::move(p), std::move(d))) {
    cfg_ = impl_->cfg;
}

ObjectIndexer::~ObjectIndexer() { stop(); }

bool ObjectIndexer::has_source(const std::string& type) const { return impl_->find(type) != nullptr; }

void ObjectIndexer::start() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->started) return;
    impl_->started = true; impl_->stopping = false;
    impl_->th = std::thread([this] { impl_->run(); });
}

void ObjectIndexer::stop() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->started) return;
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
    if (impl_->th.joinable()) impl_->th.join();
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->started = false;
}

bool ObjectIndexer::request_backfill(const BackfillRequest& r, std::string& err) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->started) { err = "the object indexer is not running"; return false; }
    if (!impl_->find(r.source)) { err = "automatic indexing is not enabled for the '" + r.source + "' source"; return false; }
    if (impl_->bf_pending || impl_->bf_active) { err = "a backfill is already queued or running"; return false; }
    if (r.frame_ids.empty() && r.start_utc_ms > 0 && r.end_utc_ms > 0 && r.end_utc_ms <= r.start_utc_ms) { err = "end must be after start"; return false; }
    impl_->bf_req = r;
    impl_->bf_req.limit = std::clamp(r.limit, 1, impl_->cfg.max_backfill_frames);
    impl_->bf_pending = true;
    impl_->st.backfill_source = r.source;
    impl_->st.backfill_state = "queued"; impl_->st.backfill_total = impl_->st.backfill_done = impl_->st.backfill_indexed = impl_->st.backfill_failed = 0;
    impl_->cv.notify_all();
    return true;
}

void ObjectIndexer::cancel_backfill() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->bf_pending) { impl_->bf_pending = false; impl_->st.backfill_state = "cancelled"; return; }
    if (impl_->bf_active) impl_->bf_cancel = true;
    impl_->cv.notify_all();
}

ObjectIndexerStats ObjectIndexer::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    ObjectIndexerStats s = impl_->st;
    for (auto& src : impl_->srcs) { IndexerSourceStats x = src->st; x.queue_depth = static_cast<int>(src->q.size()); s.sources.push_back(x); }
    if (!impl_->started && s.state != "stopped") s.state = "disabled";
    return s;
}

} // namespace visualmemory
