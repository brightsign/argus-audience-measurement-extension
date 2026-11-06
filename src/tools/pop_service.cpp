// SPDX-License-Identifier: MIT
#include "pop_service.h"
#include "display/display_frame_source.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace visualmemory;

namespace argus_pop {

namespace {
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
// Used where the store API wants a provider but nothing is embedded (failed-capture rows).
class NoProvider : public EmbeddingProvider {
public:
    EmbeddingSpec spec() const override { return {}; }
    bool embed(const ImageView&, std::vector<float>&) override { return false; }
    bool ready() const override { return false; }
};
std::string hex64(uint64_t v) { char b[20]; std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v)); return b; }
bool read_file(const std::string& p, std::vector<uint8_t>& out, size_t max = 8u << 20) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0 || static_cast<size_t>(n) > max) return false;
    out.resize(static_cast<size_t>(n)); f.seekg(0);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), n));
}
bool write_text_atomic(const fs::path& p, const std::string& s) {
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false; f << s; f.flush(); if (!f) return false; }
    fs::rename(tmp, p, ec);
    return !ec;
}
std::string csv_esc(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += "\"\""; else o += c; }
    return o + "\"";
}
json evidence_json(const Evidence& e) {
    return {{"evidence_id", e.evidence_id}, {"idx", e.idx}, {"status", e.status}, {"failure_reason", e.failure_reason}, {"source", e.source},
            {"target_offset_ms", e.target_offset_ms}, {"actual_offset_ms", e.actual_offset_ms}, {"utc_ms", e.utc_ms}, {"took_ms", e.took_ms},
            {"width", e.width}, {"height", e.height}, {"bytes", e.bytes}, {"sha256", e.sha256}, {"phash", hex64(e.phash)},
            {"image_available", e.image_available}, {"capture_method", e.capture_method}, {"provenance", e.provenance},
            {"time_uncertainty_ms", e.uncertainty_ms < 0 ? json(nullptr) : json(e.uncertainty_ms)},
            {"image_url", e.image_available ? "/api/pop/sessions/" + e.session_id + "/evidence/" + std::to_string(e.evidence_id) + "/image" : ""}};
}
json media_json(const MediaAsset& m) {
    return {{"media_id", m.media_id}, {"display_name", m.display_name}, {"kind", m.kind}, {"uri", m.uri}, {"sha256", m.sha256}, {"codec", m.codec},
            {"duration_s", m.duration_s}, {"slot_s", m.slot_s}, {"width", m.width}, {"height", m.height}, {"fps", m.fps},
            {"created_utc_ms", m.created_utc_ms}, {"updated_utc_ms", m.updated_utc_ms}, {"reference_frames", m.reference_frames}};
}
} // namespace

PopService::PopService(PopCfg cfg, std::string storage_root, ProviderRunner run)
    : cfg_(std::move(cfg)), root_(std::move(storage_root)), run_(std::move(run)) {
    PopConfig pc; pc.root_directory = root_; pc.allow_schema_upgrade = cfg_.allow_schema_upgrade;
    store_ = std::make_unique<PopStore>(pc);
}
PopService::~PopService() { stop(); }

bool PopService::start(std::string& err) {
    if (!store_->open(err)) return false;
    load_state();
    if (cfg_.enabled && !running_.load()) {
        stop_ = false; running_ = true;
        if (cfg_.history.enabled) {
            std::string he;
            if (!display_) hist_last_error_ = "display history needs visual_memory.display.enabled (OS rendered frames)";
            else if (cfg_.capture_backend != "resilience_grpc") hist_last_error_ = "display history needs visual_memory.pop.capture_backend = resilience_grpc";
            else if (!store_->history_open(he)) hist_last_error_ = "history_open: " + he;
            if (!hist_last_error_.empty()) cfg_.history.enabled = false;               // recording stays off; any history recorded earlier remains searchable
        }
        th_ = std::thread([this] { loop(); });
        if (display_) grpc_th_ = std::thread([this] { grpc_loop(); });
        if (cfg_.history.enabled) hist_th_ = std::thread([this] { history_loop(); });
    }
    return true;
}
void PopService::stop() {
    stop_ = true;
    hq_cv_.notify_all();
    if (th_.joinable()) th_.join();
    if (grpc_th_.joinable()) grpc_th_.join();
    if (hist_th_.joinable()) hist_th_.join();
    running_ = false;
}

// ---- adapter state ----------------------------------------------------------------------------------------------
void PopService::load_state() {
    std::ifstream f(fs::path(root_) / "pop" / "adapter_state.json");
    if (!f) return;
    try {
        const json j = json::parse(f);
        std::lock_guard<std::mutex> lk(mu_);
        if (j.value("events_path", std::string()) == cfg_.events_path) offset_ = j.value("offset", static_cast<uint64_t>(0));
    } catch (...) {}
}
void PopService::save_state() {
    json j = {{"events_path", cfg_.events_path}, {"offset", offset_}, {"saved_utc_ms", now_ms()}};
    write_text_atomic(fs::path(root_) / "pop" / "adapter_state.json", j.dump());
}

std::string PopService::map_path(const std::string& p) const {
    if (!cfg_.sd_from.empty() && p.rfind(cfg_.sd_from, 0) == 0) return cfg_.sd_to + p.substr(cfg_.sd_from.size());
    return p;
}

void PopService::loop() {
    int stuck = 0;
    while (!stop_) {
        std::error_code ec;
        const auto sz = fs::file_size(cfg_.events_path, ec);
        if (!ec) {
            std::lock_guard<std::mutex> lk(mu_);
            file_size_ = sz;
            if (sz < offset_) { offset_ = 0; anchor_ms_ = INT64_MAX; }               // truncated / replaced
        }
        uint64_t off;
        { std::lock_guard<std::mutex> lk(mu_); off = offset_; }
        if (!ec && sz > off) {
            std::ifstream f(cfg_.events_path, std::ios::binary);
            f.seekg(static_cast<std::streamoff>(off));
            std::string chunk(static_cast<size_t>(std::min<uint64_t>(sz - off, 1u << 20)), '\0');
            f.read(&chunk[0], static_cast<std::streamsize>(chunk.size()));
            chunk.resize(static_cast<size_t>(f.gcount()));
            size_t pos = 0;
            const int64_t read_utc = now_ms();
            while (!stop_) {
                const size_t nl = chunk.find('\n', pos);
                if (nl == std::string::npos) break;                                   // partial line: wait for the rest
                const std::string line = chunk.substr(pos, nl - pos);
                bool retry = false;
                try { process_line(line, read_utc); }
                catch (const std::exception& e) { std::lock_guard<std::mutex> lk(mu_); ++errors_; last_error_ = e.what(); }
                { std::lock_guard<std::mutex> lk(mu_); retry = last_error_.rfind("RETRY:", 0) == 0; }
                if (retry && ++stuck < 25) break;                                     // try this line again on the next poll
                stuck = 0;
                { std::lock_guard<std::mutex> lk(mu_); if (last_error_.rfind("RETRY:", 0) == 0) last_error_ = "skipped after repeated failure: " + last_error_.substr(6); }
                pos = nl + 1;
                { std::lock_guard<std::mutex> lk(mu_); offset_ = off + pos; }
                save_state();
            }
        }
        const int64_t t = now_ms();
        if (t - last_prune_ms_ > 60000) { last_prune_ms_ = t; prune(); }
        for (int i = 0; i < std::max(1, cfg_.poll_ms / 20) && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void PopService::process_line(const std::string& line, int64_t read_utc_ms) {
    if (line.empty()) return;
    json j;
    try { j = json::parse(line); } catch (...) { std::lock_guard<std::mutex> lk(mu_); ++errors_; last_error_ = "unparsable event line"; return; }
    if (!j.is_object()) return;
    const std::string ev = j.value("ev", std::string());
    const int64_t mono = j.value("mono_ms", static_cast<int64_t>(0));
    int64_t utc;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++lines_;
        if (last_error_.rfind("RETRY:", 0) == 0) last_error_.clear();
        if (ev == "boot") { anchor_ms_ = INT64_MAX; ++boots_; }
        // The player has no wall clock: UTC = anchor + mono_ms, where anchor is the smallest (read time - mono_ms)
        // seen since boot, i.e. the estimate with the least read latency (error <= adapter poll period + flush delay).
        anchor_ms_ = std::min(anchor_ms_, read_utc_ms - mono);
        utc = anchor_ms_ + mono;
    }
    const std::string sid = j.value("session", std::string());
    if (ev.empty() || sid.empty()) return;                                             // boot / job_invalid have no session

    if (ev == "capture_ok" || ev == "capture_failed") {
        if (!ingest_capture(j, utc, sid, ev == "capture_ok")) return;                  // failure recorded as RETRY:/error
    }
    PopEventIn e;
    e.session_id = sid; e.type = ev; e.utc_ms = utc; e.mono_ms = mono;
    e.job_id = j.value("job_id", std::string()); e.played_media_id = j.value("media_id", std::string());
    e.expected_media_id = j.value("expected_media_id", std::string());
    if (j.contains("reason") && j["reason"].is_string()) e.reason = j["reason"].get<std::string>();
    if (j.contains("actual_play_ms") && j["actual_play_ms"].is_number()) e.actual_play_ms = j["actual_play_ms"].get<int64_t>();
    if (j.contains("capture_count") && j["capture_count"].is_number_integer()) e.captures_requested = j["capture_count"].get<int>();
    bool grpc_session = false;
    { std::lock_guard<std::mutex> pl(plan_mu_); grpc_session = plans_.count(sid) != 0; }
    if (grpc_session && ev == "session_scheduled_run") e.captures_requested = -1;      // the player takes no screenshots for this session: keep the scheduled count
    if (grpc_session && ev == "play_started" && display_) {
        std::lock_guard<std::mutex> pl(plan_mu_);
        auto it = plans_.find(sid);
        if (it != plans_.end() && !it->second.started) {
            it->second.started = true; it->second.play_start_utc = utc; it->second.ready_at_start = display_->ready(); it->second.prewarm_released = false;
            display_->hold(sid);
            if (cfg_.history.enabled) history_begin(sid, utc, it->second.slot_ms, it->second.ready_at_start);
        }
    }
    e.detail_json = line.size() <= 4000 ? line : std::string("{}");
    std::string err;
    if (!store_->apply_event(e, err)) { std::lock_guard<std::mutex> lk(mu_); ++errors_; last_error_ = "apply_event: " + err; return; }
    { std::lock_guard<std::mutex> lk(mu_); ++events_; last_event_utc_ms_ = utc; }
    if (ev == "session_end") {
        if (grpc_session) {                                          // resolve every capture offset that was reached, then stop holding the OS producer
            grpc_collect(sid, true, utc);
            if (cfg_.history.enabled) history_end(sid, utc);
            if (display_) display_->release(sid);
            std::lock_guard<std::mutex> pl(plan_mu_);
            plans_.erase(sid);
        }
        Verification v;
        if (!store_->verify_session(sid, cfg_.thresholds, v, err)) { std::lock_guard<std::mutex> lk(mu_); ++errors_; last_error_ = "verify: " + err; }
    }
}

bool PopService::ingest_capture(const json& j, int64_t utc_ms, const std::string& sid, bool ok) {
    EvidenceIn in;
    in.session_id = sid; in.idx = j.value("idx", 0); in.target_offset_ms = j.value("target_offset_ms", 0);
    in.actual_offset_ms = j.value("actual_offset_ms", 0); in.took_ms = j.value("took_ms", 0); in.utc_ms = utc_ms; in.source = cfg_.source_id;
    std::string file;
    Evidence out; std::string err;
    if (!ok) {
        in.status = "failed"; in.failure_reason = j.value("reason", std::string("capture failed"));
        NoProvider np;
        if (!store_->add_evidence(np, in, out, err)) { std::lock_guard<std::mutex> lk(mu_); ++errors_; last_error_ = "evidence: " + err; }
        return true;
    }
    file = map_path(j.value("file", std::string()));
    // roVideoMode.Screenshot() returns true BEFORE the file is fully written (seen on the XT5: "Premature end of JPEG
    // file"). Accept the file only when it ends with the JPEG EOI marker and its size is unchanged between two reads.
    std::vector<uint8_t> bytes;
    size_t prev = 0;
    for (int i = 0; i < 40; ++i) {
        std::vector<uint8_t> b;
        if (read_file(file, b) && b.size() > 4 && b[b.size() - 2] == 0xFF && b[b.size() - 1] == 0xD9 && b.size() == prev) { bytes = std::move(b); break; }
        prev = b.size();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (bytes.empty()) {
        in.status = "failed"; in.failure_reason = "screenshot file missing, empty or incomplete 4 s after the capture was reported";
        NoProvider np; store_->add_evidence(np, in, out, err);
        std::lock_guard<std::mutex> lk(mu_); ++errors_; last_error_ = in.failure_reason;
        return true;
    }
    in.jpeg = std::move(bytes);
    std::string why; bool done = false, stored = false;
    const bool have = run_ && run_([&](EmbeddingProvider& p) { stored = store_->add_evidence(p, in, out, err); done = true; }, why);
    if (!have || !done) { std::lock_guard<std::mutex> lk(mu_); last_error_ = "RETRY:embedding unavailable: " + why; return false; }
    if (!stored) { std::lock_guard<std::mutex> lk(mu_); ++errors_; last_error_ = "evidence: " + err; return true; }
    std::error_code ec; fs::remove(file, ec);                      // the evidence store owns the image now
    return true;
}


// ---- capture backend "resilience_grpc": evidence from the OS rendered frames -----------------------------------------------------
// The player application takes no screenshots for these sessions. At play_started the display source is held (it keeps the OS producer
// capturing), and for every capture offset the frame closest to (play start + offset) is taken from the source's bounded ring. A frame is
// used only if it was received within the configured window of the target and its time uncertainty is within the limit; otherwise a
// failed capture is recorded, exactly like a failed Screenshot() (so the existing "capture unsupported" / "incomplete" outcomes apply).
void PopService::grpc_loop() {
    while (!stop_) {
        std::vector<std::string> ids;
        { std::lock_guard<std::mutex> pl(plan_mu_); for (const auto& kv : plans_) ids.push_back(kv.first); }
        for (const auto& id : ids) grpc_collect(id, false, 0);
        if (cfg_.history.enabled) history_tick();
        const int64_t t = now_ms();
        {   // plans that never started (the play never happened) or ran far past their slot are dropped, and the source released
            std::lock_guard<std::mutex> pl(plan_mu_);
            for (auto it = plans_.begin(); it != plans_.end();) {
                if (!it->second.started && !it->second.prewarm_released && t - it->second.created_ms > static_cast<int64_t>(cfg_.grpc_prewarm_s) * 1000) {
                    if (display_) display_->release(it->first);               // the player has not started: stop keeping the OS producer awake (re-held at play_started)
                    it->second.prewarm_released = true;
                }
                const bool never = !it->second.started && t - it->second.created_ms > 600000;
                const bool over = it->second.started && t - it->second.play_start_utc > it->second.slot_ms + 60000;
                if (never || over) { if (cfg_.history.enabled) history_abort(it->first); if (display_) display_->release(it->first); it = plans_.erase(it); } else ++it;
            }
        }
        for (int i = 0; i < 5 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void PopService::grpc_collect(const std::string& sid, bool final_pass, int64_t end_utc_ms) {
    if (!display_) return;
    GrpcPlan p;
    { std::lock_guard<std::mutex> pl(plan_mu_); auto it = plans_.find(sid); if (it == plans_.end() || !it->second.started) return; p = it->second; }
    const int64_t now = now_ms();
    for (size_t i = 0; i < p.offsets_ms.size(); ++i) {
        if (p.done[i]) continue;
        const int64_t tgt = p.play_start_utc + p.offsets_ms[i];
        // An offset whose window reaches the nominal end of the slot is decided in the final pass, when the real end of the play is known (a frame
        // captured around the end may already show what comes after the advertisement).
        if (!final_pass && p.offsets_ms[i] + cfg_.grpc_after_ms + cfg_.grpc_end_guard_ms > p.slot_ms) continue;
        if (final_pass) {
            if (tgt > end_utc_ms) { std::lock_guard<std::mutex> pl(plan_mu_); plans_[sid].done[i] = 1; continue; }      // the play ended before this offset: no capture, as with screenshots
        } else if (now < tgt + 300) continue;                                          // wait until frames just after the target exist, then pick the closest
        bool done = false;
        const int idx = static_cast<int>(i) + 1;
        if (p.break_capture) {
            done = grpc_record(sid, idx, p.offsets_ms[i], "display capture disabled by the break_capture test hook", nullptr, p.play_start_utc);
        } else {
            // A frame is evidence only if it cannot belong to the time before the play or after it. Its true display moment lies in
            // [capture - uncertainty, capture]; the start of the play is known to within the adapter's event timing (start guard).
            const int64_t earliest = p.play_start_utc + cfg_.grpc_start_guard_ms;
            const int64_t latest = (final_pass && end_utc_ms > 0) ? end_utc_ms - cfg_.grpc_end_guard_ms : INT64_MAX;
            auto usable = [&](const argus_display::DisplayFrame& f) {
                return f.capture_utc_ms - f.uncertainty_ms >= earliest && f.capture_utc_ms <= latest && f.uncertainty_ms <= cfg_.grpc_max_uncertainty_ms;
            };
            auto fr = display_->nearest_if(tgt, cfg_.grpc_before_ms, cfg_.grpc_after_ms, usable);
            if (fr) {
                done = grpc_record(sid, idx, p.offsets_ms[i], "", &*fr, p.play_start_utc);
            } else if (final_pass || now > tgt + cfg_.grpc_after_ms + 500) {
                auto any = display_->nearest(tgt, cfg_.grpc_before_ms, cfg_.grpc_after_ms);
                const auto st = display_->status();
                std::string why;
                if (any && any->uncertainty_ms > cfg_.grpc_max_uncertainty_ms) why = "display frame time uncertainty " + std::to_string(any->uncertainty_ms) + " ms exceeds the limit";
                else if (any) why = "transition-adjacent: the only frames near the target may belong to the time before or after the play (timing uncertain by up to " + std::to_string(any->uncertainty_ms) + " ms)";
                else why = "no fresh display frame within " + std::to_string(cfg_.grpc_before_ms) + "/" + std::to_string(cfg_.grpc_after_ms) + " ms of the target (display source: " + st.value("state", std::string("unknown")) + ")";
                done = grpc_record(sid, idx, p.offsets_ms[i], why, nullptr, p.play_start_utc);
            }
        }
        if (done) { std::lock_guard<std::mutex> pl(plan_mu_); auto it = plans_.find(sid); if (it != plans_.end()) it->second.done[i] = 1; }
    }
}

bool PopService::grpc_record(const std::string& sid, int idx, int64_t target_ms, const std::string& why, const argus_display::DisplayFrame* frame, int64_t play_start_utc) {
    EvidenceIn in;
    in.session_id = sid; in.idx = idx; in.target_offset_ms = static_cast<int>(target_ms); in.source = cfg_.source_id;
    in.capture_method = "resilience_grpc";
    Evidence out; std::string err;
    if (!frame) {
        in.status = "failed"; in.failure_reason = why; in.actual_offset_ms = 0; in.utc_ms = now_ms();
        NoProvider np;
        if (!store_->add_evidence(np, in, out, err)) { std::lock_guard<std::mutex> lk(mu_); ++errors_; last_error_ = "evidence: " + err; }
        return true;
    }
    in.actual_offset_ms = static_cast<int>(frame->capture_utc_ms - play_start_utc);
    in.took_ms = static_cast<int>(frame->acquire_ms); in.utc_ms = frame->capture_utc_ms; in.uncertainty_ms = static_cast<int>(frame->uncertainty_ms);
    bool ready0 = false;
    { std::lock_guard<std::mutex> pl(plan_mu_); auto it = plans_.find(sid); if (it != plans_.end()) ready0 = it->second.ready_at_start; }
    in.provenance = "weston_vop2_writeback p" + std::to_string(frame->provider) + "; ready_at_start=" + (ready0 ? "1" : "0") + "; time bound=rpc+configured budget";
    in.jpeg.assign(frame->jpeg->begin(), frame->jpeg->end());
    std::string why2; bool done = false, stored = false;
    const bool have = run_ && run_([&](EmbeddingProvider& p) { stored = store_->add_evidence(p, in, out, err); done = true; }, why2);
    if (!have || !done) return false;                                                        // embedding provider busy: the frame may still be in the ring next cycle
    if (!stored) { std::lock_guard<std::mutex> lk(mu_); ++errors_; last_error_ = "evidence: " + err; }
    return true;
}

// ---- display history (Phase 2): about one display frame per second while a registered session plays ------------------------------------------
// Separate from the verification checkpoints above: nothing here touches display_capture_evidence or the verifier. The sampler runs on the grpc
// thread (pure ring lookups, no I/O); frames are staged in memory until commit_delay_ms after their capture so that the session_end event can still
// cancel frames that belong to the time after the play; a bounded queue hands the committed frames to a writer thread that stores the original JPEG
// bytes and, when the NPU is free, computes the vectors. Targets are session-relative, scheduled on the monotonic clock.
void PopService::history_begin(const std::string& sid, int64_t start_utc, int64_t slot_ms, bool ready_at_start) {
    PopSession ps; const bool have = store_->get_session(sid, ps);
    HistSession hs; hs.expected = have ? ps.expected_media_id : std::string(); hs.start_utc = start_utc; hs.slot_ms = slot_ms; hs.ready_at_start = ready_at_start;
    hs.start_mono = argus_display::mono_ms() - (now_ms() - start_utc);        // the play start on the monotonic clock
    // A target whose whole window lies before the earliest usable display moment (start guard + the producer latency budget) can never be served:
    // skip it instead of counting a miss in every session.
    const int64_t first_usable = cfg_.grpc_start_guard_ms + (display_ ? display_->cfg().producer_latency_ms : 250) + 5;
    while (static_cast<int64_t>(cfg_.history.phase_ms) + static_cast<int64_t>(hs.next_k) * cfg_.history.interval_ms + cfg_.history.window_ms < first_usable) ++hs.next_k;
    std::lock_guard<std::mutex> lk(hist_mu_);
    hist_sessions_[sid] = std::move(hs);
}

bool PopService::history_offer(HistJob j) {
    {
        std::lock_guard<std::mutex> lk(hq_mu_);
        if (!j.finalize && static_cast<int>(hq_.size()) >= cfg_.history.queue_capacity) { ++hc_.dropped_queue_full; return false; }
        hq_.push_back(std::move(j));
    }
    hq_cv_.notify_one();
    return true;
}

// Takes the samples whose target is due. final_pass: the session has ended at end_utc, so only frames that were captured at least end_guard before the
// end may be used (a later frame can show what comes after the advertisement); otherwise the end is still unknown.
void PopService::history_sample(HistSession& hs, const std::string& sid, int64_t now_mono, bool final_pass, int64_t end_utc) {
    const auto& h = cfg_.history;
    const int64_t elapsed = now_mono - hs.start_mono;
    const int64_t earliest = hs.start_utc + cfg_.grpc_start_guard_ms;
    const int64_t latest = final_pass && end_utc > 0 ? end_utc - cfg_.grpc_end_guard_ms : INT64_MAX;
    auto usable = [&](const argus_display::DisplayFrame& f) {
        return f.capture_utc_ms - f.uncertainty_ms >= earliest && f.capture_utc_ms <= latest && f.uncertainty_ms <= cfg_.grpc_max_uncertainty_ms;
    };
    while (hs.next_k < h.max_frames_per_session) {
        const int64_t off = static_cast<int64_t>(h.phase_ms) + static_cast<int64_t>(hs.next_k) * h.interval_ms;
        if (final_pass) { if (end_utc > 0 && hs.start_utc + off > end_utc) break; }                       // the target lies after the end of the play
        else {
            if (off + h.window_ms + cfg_.grpc_end_guard_ms > hs.slot_ms) break;                          // near the nominal end: decided by the final pass
            if (elapsed < off + h.window_ms + 50) break;                                                  // wait until frames just after the target exist
        }
        ++hc_.targets;
        auto fr = display_->nearest_if(hs.start_utc + off, h.window_ms, h.window_ms, usable);
        if (!fr) { ++hs.missed; ++hc_.missed_no_frame; ++hs.next_k; continue; }
        if (fr->seq <= hs.last_seq) { ++hs.duplicates; ++hc_.duplicate_frames; ++hs.next_k; continue; }    // the source delivered no new frame for this second
        HistStaged st; st.k = hs.next_k; st.target_off = static_cast<int>(off); st.jpeg = fr->jpeg; st.capture_utc = fr->capture_utc_ms;
        st.capture_mono = fr->capture_mono_ms; st.uncertainty = fr->uncertainty_ms; st.seq = fr->seq; st.provider = fr->provider;
        hs.last_seq = fr->seq; hs.staged.push_back(std::move(st)); ++hc_.staged; ++hs.next_k;
    }
}

void PopService::history_tick() {
    const int64_t now_mono = argus_display::mono_ms();
    std::vector<HistJob> out;
    {
        std::lock_guard<std::mutex> lk(hist_mu_);
        for (auto& kv : hist_sessions_) {
            HistSession& hs = kv.second;
            history_sample(hs, kv.first, now_mono, false, 0);
            while (!hs.staged.empty() && now_mono - hs.staged.front().capture_mono >= cfg_.history.commit_delay_ms) {
                const HistStaged& st = hs.staged.front();
                HistJob j; j.sid = kv.first; j.in.session_id = kv.first; j.in.sec_idx = st.k; j.in.target_offset_ms = st.target_off;
                j.in.actual_offset_ms = static_cast<int>(st.capture_utc - hs.start_utc); j.in.utc_ms = st.capture_utc; j.in.mono_ms = st.capture_mono;
                j.in.uncertainty_ms = static_cast<int>(st.uncertainty); j.in.source = cfg_.source_id; j.in.expected_media_id = hs.expected;
                j.in.provenance = "weston_vop2_writeback p" + std::to_string(st.provider) + "; history " + std::to_string(1000 / std::max(1, cfg_.history.interval_ms)) + "fps; ready_at_start=" +
                                  (hs.ready_at_start ? "1" : "0") + "; time bound=rpc+configured budget";
                j.in.jpeg.assign(st.jpeg->begin(), st.jpeg->end());
                out.push_back(std::move(j)); hs.staged.pop_front(); ++hs.enqueued;
            }
        }
    }
    for (auto& j : out) if (history_offer(std::move(j))) ++hc_.queued;
}

void PopService::history_end(const std::string& sid, int64_t end_utc) {
    HistSession hs;
    {
        std::lock_guard<std::mutex> lk(hist_mu_);
        auto it = hist_sessions_.find(sid);
        if (it == hist_sessions_.end()) return;
        hs = std::move(it->second); hist_sessions_.erase(it);
    }
    history_sample(hs, sid, argus_display::mono_ms(), true, end_utc);          // targets that were still open when the play ended
    PopSession ps; store_->get_session(sid, ps);
    const int64_t latest = end_utc - cfg_.grpc_end_guard_ms;
    std::vector<HistStaged> keep;
    for (auto& st : hs.staged) { if (st.capture_utc <= latest) keep.push_back(std::move(st)); else { ++hs.dropped_after_end; ++hc_.dropped_after_end; } }
    for (auto& st : keep) {
        HistJob j; j.sid = sid; j.in.session_id = sid; j.in.sec_idx = st.k; j.in.target_offset_ms = st.target_off;
        j.in.actual_offset_ms = static_cast<int>(st.capture_utc - hs.start_utc); j.in.utc_ms = st.capture_utc; j.in.mono_ms = st.capture_mono;
        j.in.uncertainty_ms = static_cast<int>(st.uncertainty); j.in.source = cfg_.source_id; j.in.expected_media_id = hs.expected;
        j.in.provenance = "weston_vop2_writeback p" + std::to_string(st.provider) + "; history " + std::to_string(1000 / std::max(1, cfg_.history.interval_ms)) + "fps; ready_at_start=" +
                          (hs.ready_at_start ? "1" : "0") + "; time bound=rpc+configured budget";
        j.in.jpeg.assign(st.jpeg->begin(), st.jpeg->end());
        if (history_offer(std::move(j))) ++hc_.queued;
    }
    HistJob f; f.finalize = true; f.sid = sid; f.end_utc = end_utc; f.played = ps.played_media_id;
    history_offer(std::move(f));
}

void PopService::history_abort(const std::string& sid) {
    std::lock_guard<std::mutex> lk(hist_mu_);
    hist_sessions_.erase(sid);                                                   // the session never ended cleanly: staged frames are discarded
}

void PopService::history_loop() {
    const auto& h = cfg_.history;
    auto set_err = [&](const std::string& m) { std::lock_guard<std::mutex> lk(mu_); hist_last_error_ = m; };
    int64_t last_embed = 0;
    HistoryLimits lim; lim.max_frames_per_session = h.max_frames_per_session; lim.max_pending_embeddings = h.max_pending_embeddings;
    while (true) {
        HistJob j; bool have = false;
        {
            std::unique_lock<std::mutex> lk(hq_mu_);
            if (hq_.empty() && !stop_) hq_cv_.wait_for(lk, std::chrono::milliseconds(100));
            if (!hq_.empty()) { j = std::move(hq_.front()); hq_.pop_front(); have = true; }
            else if (stop_) break;
        }
        if (have) {
            if (j.finalize) { store_->history_finalize_session(j.sid, j.end_utc, cfg_.grpc_end_guard_ms, j.played); continue; }
            struct statvfs vs{};
            if (statvfs(root_.c_str(), &vs) == 0 && static_cast<uint64_t>(vs.f_bavail) * vs.f_frsize < h.min_free_bytes) { ++hc_.paused_low_space; set_err("paused: free space below min_free_bytes"); continue; }
            const auto t0 = std::chrono::steady_clock::now();
            HistoryFrame out; std::string err;
            const bool ok = store_->add_history_frame(j.in, lim, out, err);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ok) {
                ++hc_.written; if (out.evidence_id) ++hc_.reused_checkpoint; hc_.last_commit_utc_ms = out.utc_ms;
                hc_.last_write_ms_x100 = static_cast<int64_t>(ms * 100);
                if (static_cast<int64_t>(ms * 100) > hc_.max_write_ms_x100) hc_.max_write_ms_x100 = static_cast<int64_t>(ms * 100);
            }
            else if (err == "duplicate") ++hc_.rejected_duplicate;
            else if (err == "session limit") ++hc_.session_limit;
            else if (err == "embedding backlog") ++hc_.backlog_refused;
            else if (err.rfind("not a complete JPEG", 0) == 0 || err == "unknown session") ++hc_.rejected_invalid;
            else if (err.find("write") != std::string::npos || err.find("cannot create") != std::string::npos || err.find("rename") != std::string::npos) { ++hc_.write_failed; set_err(err); }
            else { ++hc_.sqlite_failed; set_err(err); }
            continue;                                                                // keep draining before the NPU work
        }
        const int64_t t = now_ms();
        if (t - last_embed >= 50 && store_->history_pending_embeddings() > 0) {
            last_embed = t;
            int n = 0; std::string err, why;
            const auto t0 = std::chrono::steady_clock::now();
            const bool got = run_ && run_([&](EmbeddingProvider& p) { n = store_->embed_pending_history(p, h.embed_batch, err); }, why);
            if (!got) { ++hc_.embed_busy; continue; }
            if (n > 0) { hc_.embedded += n; hc_.last_embed_ms_x100 = static_cast<int64_t>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() * 100 / n); }
            else if (n < 0) { ++hc_.embed_failed; set_err("embed: " + err); }
        }
    }
}

void PopService::history_flush_for_test() {
    for (int i = 0; i < 400; ++i) {
        { std::lock_guard<std::mutex> lk(hq_mu_); if (hq_.empty() && store_->history_pending_embeddings() == 0) return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

json PopService::history_frames(const std::string& session_id, int limit, int offset) {
    uint64_t total = 0;
    const auto rows = store_->list_history(session_id, limit, offset, &total);
    json arr = json::array();
    for (const auto& r : rows) {
        arr.push_back({{"frame_id", r.frame_id}, {"session_id", r.session_id}, {"sec_idx", r.sec_idx}, {"role", r.role}, {"verification_checkpoint_evidence_id", r.evidence_id ? json(r.evidence_id) : json(nullptr)},
                       {"source", r.source}, {"expected_media_id", r.expected_media_id}, {"played_media_id", r.played_media_id}, {"capture_method", r.capture_method},
                       {"provenance", r.provenance}, {"utc_ms", r.utc_ms}, {"mono_ms", r.mono_ms}, {"target_offset_ms", r.target_offset_ms}, {"actual_offset_ms", r.actual_offset_ms},
                       {"time_uncertainty_ms", r.uncertainty_ms < 0 ? json(nullptr) : json(r.uncertainty_ms)}, {"width", r.width}, {"height", r.height}, {"bytes", r.bytes}, {"sha256", r.sha256},
                       {"embedded", r.embedded}, {"image_available", r.image_available || r.evidence_id != 0},
                       {"image_url", (r.image_available || r.evidence_id != 0) ? "/api/pop/history/" + std::to_string(r.frame_id) + "/image" : ""}});
    }
    return {{"frames", arr}, {"total", total}, {"limit", limit}, {"offset", offset}};
}

json PopService::history_status() {
    const auto& h = cfg_.history;
    const HistoryStats st = store_->history_stats();
    json active = json::array();
    {
        std::lock_guard<std::mutex> lk(hist_mu_);
        for (const auto& kv : hist_sessions_)
            active.push_back({{"session_id", kv.first}, {"targets_taken", kv.second.next_k}, {"staged", kv.second.staged.size()}, {"queued_for_storage", kv.second.enqueued},
                              {"missed", kv.second.missed}, {"duplicates", kv.second.duplicates}, {"expected_total", kv.second.slot_ms > 0 ? kv.second.slot_ms / std::max(1, h.interval_ms) : 0}});
    }
    size_t qd; { std::lock_guard<std::mutex> lk(hq_mu_); qd = hq_.size(); }
    struct statvfs vs{}; uint64_t freeb = 0, totalb = 0;
    if (statvfs(root_.c_str(), &vs) == 0) { freeb = static_cast<uint64_t>(vs.f_bavail) * vs.f_frsize; totalb = static_cast<uint64_t>(vs.f_blocks) * vs.f_frsize; }
    std::lock_guard<std::mutex> lk(mu_);
    return {{"enabled", h.enabled}, {"ready", st.ready}, {"recording_scope", "registered Proof-of-Play sessions only (never the idle presentation)"},
            {"interval_ms", h.interval_ms}, {"frames_per_second_target", 1000.0 / std::max(1, h.interval_ms)}, {"phase_ms", h.phase_ms}, {"window_ms", h.window_ms},
            {"commit_delay_ms", h.commit_delay_ms}, {"active_sessions", active}, {"queue", {{"depth", qd}, {"capacity", h.queue_capacity}}},
            {"counters", {{"targets", hc_.targets.load()}, {"staged", hc_.staged.load()}, {"queued", hc_.queued.load()}, {"written", hc_.written.load()},
                          {"reused_checkpoint", hc_.reused_checkpoint.load()}, {"embedded", hc_.embedded.load()}, {"missed_no_frame", hc_.missed_no_frame.load()},
                          {"duplicate_frames", hc_.duplicate_frames.load()}, {"dropped_queue_full", hc_.dropped_queue_full.load()}, {"dropped_after_end", hc_.dropped_after_end.load()},
                          {"rejected_invalid", hc_.rejected_invalid.load()}, {"rejected_duplicate", hc_.rejected_duplicate.load()}, {"session_limit", hc_.session_limit.load()},
                          {"backlog_refused", hc_.backlog_refused.load()}, {"write_failed", hc_.write_failed.load()}, {"sqlite_failed", hc_.sqlite_failed.load()},
                          {"embed_failed", hc_.embed_failed.load()}, {"embed_busy", hc_.embed_busy.load()}, {"paused_low_space", hc_.paused_low_space.load()}}},
            {"latency_ms", {{"last_store", hc_.last_write_ms_x100.load() / 100.0}, {"max_store", hc_.max_write_ms_x100.load() / 100.0}, {"last_embed", hc_.last_embed_ms_x100.load() / 100.0}}},
            {"store", {{"rows", st.rows}, {"embedded", st.embedded}, {"pending_embed", st.pending_embed}, {"embed_failed_rows", st.embed_failed}, {"reused_checkpoint_rows", st.linked_to_checkpoint},
                       {"images_retained", st.images_retained}, {"image_bytes", st.image_bytes}, {"sessions", st.sessions},
                       {"oldest_utc_ms", st.oldest_utc_ms ? json(st.oldest_utc_ms) : json(nullptr)}, {"newest_utc_ms", st.newest_utc_ms ? json(st.newest_utc_ms) : json(nullptr)}}},
            {"limits", {{"max_bytes", h.max_bytes}, {"jpeg_retention_days", h.jpeg_retention_days}, {"metadata_retention_days", h.metadata_retention_days}, {"max_rows", h.max_rows},
                        {"max_frames_per_session", h.max_frames_per_session}, {"min_free_bytes", h.min_free_bytes}, {"max_pending_embeddings", h.max_pending_embeddings}}},
            {"filesystem", {{"free_bytes", freeb}, {"total_bytes", totalb}}}, {"last_commit_utc_ms", hc_.last_commit_utc_ms.load() ? json(hc_.last_commit_utc_ms.load()) : json(nullptr)},
            {"last_error", hist_last_error_}};
}

// ---- operations -------------------------------------------------------------------------------------------------
bool PopService::schedule(const json& b, json& out, int& http, std::string& err) {
    http = 400;
    if (!b.is_object()) { err = "expected a JSON object"; return false; }
    const std::string expected = b.value("expected_media_id", std::string());
    MediaAsset exp;
    if (!store_->get_media(expected, exp)) { err = "expected_media_id is not in the reference library"; http = 404; return false; }
    std::string play = b.value("play_media_id", expected);
    const bool test_hooks = b.contains("play_media_id") || b.contains("interrupt_at_s") || b.value("break_capture", false);
    if (test_hooks && !cfg_.allow_test_jobs) { err = "play_media_id / interrupt_at_s / break_capture need visual_memory.pop.allow_test_jobs"; http = 403; return false; }
    MediaAsset pm;
    if (!store_->get_media(play, pm)) { err = "play_media_id is not in the reference library"; http = 404; return false; }
    double slot = b.value("slot_s", exp.slot_s);
    if (!(slot >= 1 && slot <= 3600)) { err = "slot_s must be 1..3600"; return false; }
    std::vector<int64_t> offs;
    const bool dense = b.contains("capture_every_s");
    if (dense) {
        if (b.contains("capture_offsets_s")) { err = "give capture_every_s or capture_offsets_s, not both"; return false; }
        const double every = b.value("capture_every_s", 0.0);
        if (!(every >= 0.5 && every <= slot)) { err = "capture_every_s must be 0.5..slot_s"; return false; }
        for (double t = every; t <= slot - 1.0 + 1e-9; t += every) offs.push_back(static_cast<int64_t>(t * 1000 + 0.5));
        if (offs.empty() || static_cast<int>(offs.size()) > cfg_.grpc_max_dense_captures) { err = "capture_every_s would give 0 or more than " + std::to_string(cfg_.grpc_max_dense_captures) + " captures"; return false; }
    } else if (b.contains("capture_offsets_s")) {
        if (!b["capture_offsets_s"].is_array() || b["capture_offsets_s"].size() > 10) { err = "capture_offsets_s must be an array of at most 10 numbers"; return false; }
        for (const auto& v : b["capture_offsets_s"]) {
            if (!v.is_number() || v.get<double>() < 0 || v.get<double>() > slot) { err = "capture offsets must be within the slot"; return false; }
            offs.push_back(static_cast<int64_t>(v.get<double>() * 1000));
        }
    } else for (double f : {0.1, 0.5, 0.9}) offs.push_back(static_cast<int64_t>(f * slot * 1000));
    double interrupt = b.value("interrupt_at_s", -1.0);
    if (b.contains("interrupt_at_s") && !(interrupt >= 0 && interrupt < slot)) { err = "interrupt_at_s must be within the slot"; return false; }

    std::string sid = b.value("session_id", std::string());
    if (sid.empty()) {
        std::random_device rd; char buf[48];
        std::snprintf(buf, sizeof(buf), "s-%lld-%04x", static_cast<long long>(now_ms()), static_cast<unsigned>(rd() & 0xffff));
        sid = buf;
    }
    const std::string backend = b.value("capture_backend", cfg_.capture_backend);
    if (backend != "brightscript_screenshot" && backend != "resilience_grpc") { err = "capture_backend must be brightscript_screenshot or resilience_grpc"; return false; }
    const bool grpc = backend == "resilience_grpc";
    if (dense && !(backend == "resilience_grpc")) { err = "capture_every_s is only available with capture_backend resilience_grpc"; return false; }
    if (grpc && !store_->schema_v2()) {
        err = "pop.db is schema 1 and is left untouched: set visual_memory.pop.allow_schema_upgrade to use capture_backend resilience_grpc"; http = 409; return false;
    }
    if (grpc && (!display_ || !display_->cfg().enabled)) {
        err = "capture_backend resilience_grpc needs visual_memory.display.enabled and the argus_display_bridge (OS Resilience rendered frames)"; http = 503; return false;
    }
    bool prewarmed = false;
    if (grpc) {
        // Wake the OS producer now (it only captures while interest is signalled) and require a genuinely FRESH frame before the session is accepted, so the
        // first seconds of the play are never lost to the producer's start-up (it answers UNAVAILABLE/STALE for ~1 s after waking).
        const bool require_ready = b.value("require_ready", true);
        if (!require_ready && !cfg_.allow_test_jobs) { err = "require_ready:false needs visual_memory.pop.allow_test_jobs"; http = 403; return false; }
        display_->hold(sid); prewarmed = true;
        bool ok = false; int64_t age = -1;
        for (int waited = 0; waited <= cfg_.grpc_ready_timeout_ms && !ok; waited += 50) {
            ok = display_->ready(&age);
            if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!ok && require_ready) {
            display_->release(sid);
            const auto st = display_->status();
            err = "display capture is not ready (OS service state: " + st.value("state", std::string("unknown")) + "): " + st.value("detail", std::string()) +
                  ". No session was scheduled; check /api/display/status";
            http = 503; return false;
        }
    }
    PopSession s; s.session_id = sid; s.job_id = "job-" + sid; s.expected_media_id = expected; s.slot_ms = static_cast<int64_t>(slot * 1000);
    s.captures_requested = static_cast<int>(offs.size()); s.source = cfg_.source_id; s.scheduled_utc_ms = now_ms();
    if (!store_->schedule_session(s, err)) { if (prewarmed) display_->release(sid); http = err.find("exists") != std::string::npos ? 409 : 400; return false; }

    json job = {{"job_id", s.job_id}, {"session_id", sid}, {"media_id", play}, {"expected_media_id", expected}, {"media_file", pm.uri},
                {"max_play_ms", s.slot_ms}, {"capture_offsets_ms", grpc ? std::vector<int64_t>{} : offs}, {"capture_width", 640}, {"capture_height", 400},
                {"capture_backend", backend},
                {"loop", pm.kind == "file" && pm.duration_s > 0 && pm.duration_s * 1000 < static_cast<double>(s.slot_ms)}};
    if (interrupt >= 0) job["interrupt_at_ms"] = static_cast<int64_t>(interrupt * 1000);
    if (b.value("break_capture", false) && !grpc) job["capture_dir"] = "SD:/pop/no_such_dir";        // a real, failing Screenshot() call
    char name[96]; std::snprintf(name, sizeof(name), "%013lld_%s.json", static_cast<long long>(now_ms()), sid.c_str());
    if (!write_text_atomic(fs::path(cfg_.jobs_dir) / name, job.dump())) { if (prewarmed) display_->release(sid); err = "cannot write the job file into " + cfg_.jobs_dir; http = 503; return false; }
    if (grpc) {
        GrpcPlan gp; gp.offsets_ms = offs; gp.done.assign(offs.size(), 0); gp.slot_ms = s.slot_ms; gp.created_ms = now_ms(); gp.break_capture = b.value("break_capture", false);
        std::lock_guard<std::mutex> pl(plan_mu_);
        plans_[sid] = std::move(gp);
    }
    http = 201;
    out = {{"session_id", sid}, {"job_file", name}, {"expected_media_id", expected}, {"play_media_id", play}, {"slot_ms", s.slot_ms},
           {"capture_offsets_ms", offs}, {"capture_backend", backend}, {"status", "scheduled"}};
    return true;
}

bool PopService::build_references(const std::string& media_id, json& out, int& http, std::string& err, bool replace, const std::string& profile) {
    http = 400;
    if (profile != "screenshot" && profile != "display_letterbox") { err = "profile must be screenshot or display_letterbox"; return false; }
    MediaAsset m;
    if (!store_->get_media(media_id, m)) { err = "unknown media_id"; http = 404; return false; }
    const bool disp = profile == "display_letterbox";
    const std::string target = disp ? media_id + visualmemory::kDisplayProfileSuffix() : media_id;
    if (disp) {
        if (target.size() > 64) { err = "media_id too long for the display_letterbox profile (max 46 characters)"; return false; }
        MediaAsset d = m; d.media_id = target; d.display_name = m.display_name + " (display frames)";
        if (!store_->register_media(d, err)) return false;                  // hidden profile row; keeps existing frames
    }
    const fs::path dir = fs::path(root_) / "pop" / "refs_incoming" / (disp ? media_id + ".display_letterbox" : media_id);
    std::vector<std::pair<int, fs::path>> files;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path p = it->path();
        if (p.extension() != ".jpg") continue;
        const std::string stem = p.stem().string();
        if (stem.empty() || stem.find_first_not_of("0123456789") != std::string::npos || stem.size() > 9) continue;
        files.push_back({std::atoi(stem.c_str()), p});
    }
    if (files.empty()) { err = "no <offset_ms>.jpg files in " + dir.string(); http = 404; return false; }
    if (files.size() > 1500) { err = "more than 1500 reference files"; return false; }
    std::sort(files.begin(), files.end());
    if (replace) store_->clear_references(target);                    // live sources: refresh, do not accumulate stale scenes
    int added = 0, failed = 0; json errors = json::array();
    std::string why;
    const bool have = run_ && run_([&](EmbeddingProvider& p) {
        for (const auto& f : files) {
            std::vector<uint8_t> bytes; std::string e;
            if (!read_file(f.second.string(), bytes) || !store_->add_reference(p, target, f.first, bytes, e)) {
                ++failed; if (errors.size() < 5) errors.push_back(f.second.filename().string() + ": " + e);
            } else { ++added; std::error_code rc; fs::remove(f.second, rc); }
        }
    }, why);
    if (!have) { err = "embedding provider unavailable: " + why; http = 503; return false; }
    MediaAsset after; store_->get_media(target, after);
    out = {{"media_id", media_id}, {"profile", profile}, {"added", added}, {"failed", failed}, {"errors", errors}, {"reference_frames", after.reference_frames}};
    http = 200;
    return true;
}

bool PopService::verify(const std::string& sid, json& out, int& http, std::string& err) {
    Verification v;
    if (!store_->verify_session(sid, cfg_.thresholds, v, err)) { http = err == "unknown session" ? 404 : 500; return false; }
    out = json::parse(store_->get_verification_json(sid), nullptr, false);
    http = 200;
    return true;
}

bool PopService::search_display(const std::vector<uint8_t>& jpeg, const std::map<std::string, std::string>& q, json& out, int& http, std::string& err) {
    EvidenceSearchOptions o; o.min_similarity = cfg_.thresholds.match_sim;
    auto num = [&](const char* k, double& v) -> int {                 // 0 absent, 1 ok, -1 invalid
        auto it = q.find(k); if (it == q.end() || it->second.empty()) return 0;
        char* e = nullptr; v = std::strtod(it->second.c_str(), &e); return (*e || !std::isfinite(v)) ? -1 : 1;
    };
    double v = 0;
    if (const int r = num("top_k", v)) { if (r < 0 || v < 1 || v > 50 || v != std::floor(v)) { http = 400; err = "top_k must be 1..50"; return false; } o.top_k = static_cast<int>(v); } else o.top_k = 10;
    if (const int r = num("min_similarity", v)) { if (r < 0 || v < -1 || v > 1) { http = 400; err = "min_similarity must be in [-1,1]"; return false; } o.min_similarity = static_cast<float>(v); }
    if (const int r = num("start", v)) { if (r < 0 || v < 0) { http = 400; err = "bad start"; return false; } o.start_utc_ms = static_cast<int64_t>(v); }
    if (const int r = num("end", v)) { if (r < 0 || v < 0) { http = 400; err = "bad end"; return false; } o.end_utc_ms = static_cast<int64_t>(v); }
    if (o.start_utc_ms && o.end_utc_ms && o.end_utc_ms <= o.start_utc_ms) { http = 400; err = "end must be after start"; return false; }
    if (q.count("media_id")) o.media_id = q.at("media_id");
    if (!o.media_id.empty() && o.media_id.size() > 64) { http = 400; err = "bad media_id"; return false; }
    if (q.count("method")) {                                           // resilience_grpc (Weston/VOP2, default) | brightscript_screenshot | any
        const std::string& m = q.at("method");
        if (m == "any") o.capture_method.clear(); else if (m == "resilience_grpc" || m == "brightscript_screenshot") o.capture_method = m;
        else { http = 400; err = "method must be resilience_grpc, brightscript_screenshot or any"; return false; }
    }
    o.require_image = q.count("require_image") && q.at("require_image") == "1";
    if (q.count("session_id") && !q.at("session_id").empty()) {
        o.session_id = q.at("session_id");
        if (o.session_id.size() > 64 || o.session_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) { http = 400; err = "bad session_id"; return false; }
    }
    if (q.count("role") && !q.at("role").empty() && q.at("role") != "any") {
        o.role = q.at("role");
        if (o.role != "verification_checkpoint" && o.role != "history_sample") { http = 400; err = "role must be any, verification_checkpoint or history_sample"; return false; }
    }
    if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) { http = 400; err = "body must be a JPEG image"; return false; }
    // Reference geometry: "as_uploaded" (default, the image is embedded exactly as sent) or "display" (the validated display_letterbox transform).
    std::string geometry = "as_uploaded";
    if (q.count("geometry")) {
        geometry = q.at("geometry");
        if (geometry != "as_uploaded" && geometry != "display") { http = 400; err = "geometry must be as_uploaded or display"; return false; }
    }
    int up_w = 0, up_h = 0; std::vector<uint8_t> transformed;
    const std::vector<uint8_t>* query_jpeg = &jpeg;
    if (geometry == "display") {
        std::string terr;
        if (!display_letterbox_jpeg(jpeg, transformed, up_w, up_h, terr)) { http = 400; err = "cannot apply the display geometry: " + terr; return false; }
        query_jpeg = &transformed;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    o.cancel = [deadline] { return std::chrono::steady_clock::now() > deadline; };
    EvidenceSearchResult res; std::string why, serr; bool ran = false, ok = false;
    const bool have = run_ && run_([&](EmbeddingProvider& p) { ran = true; ok = store_->search_evidence(p, *query_jpeg, o, res, serr); out["model"] = {{"name", p.spec().model_name}, {"version", p.spec().model_version},
                                                                          {"preprocessing", p.spec().preprocessing}, {"dimension", p.spec().dimension}}; }, why);
    if (!have) { http = why == "NPU busy" ? 429 : 503; err = why.empty() ? "embedding model unavailable" : why; return false; }
    if (!ran || !ok) { http = 400; err = serr.empty() ? "search failed" : serr; return false; }
    if (res.cancelled) { http = 504; err = "the search timed out"; return false; }

    auto hit_json = [&](const EvidenceHit& h, int rank) {
        const Evidence& e = h.evidence; PopSession s; Verification ver; MediaAsset ma;
        const bool hs = store_->get_session(e.session_id, s);
        const bool hv = store_->get_verification(e.session_id, ver);
        json j = evidence_json(e);
        j.erase("sha256"); j.erase("phash"); j.erase("bytes"); j.erase("took_ms");
        j["role"] = e.role;
        j["role_label"] = e.role == "history_sample" ? "Display-history sample (searchable, not a verification checkpoint)" : "Verification checkpoint";
        if (e.role == "history_sample") {
            j["history_id"] = e.history_id; j["sec_idx"] = e.sec_idx; j["evidence_id"] = nullptr; j["idx"] = nullptr;
            j["image_url"] = e.image_available ? "/api/pop/history/" + std::to_string(e.history_id) + "/image" : "";
        }
        j["rank"] = rank; j["similarity"] = h.similarity; j["session_id"] = e.session_id; j["capture_utc_ms"] = e.utc_ms;
        j["offset_in_play_ms"] = e.actual_offset_ms;
        j["session_link"] = "#pop?session=" + e.session_id;
        if (hs) {
            j["expected_media_id"] = s.expected_media_id; j["played_media_id"] = s.played_media_id;
            j["advertisement_id"] = !s.played_media_id.empty() ? s.played_media_id : s.expected_media_id;
            j["session_start_utc_ms"] = s.start_utc_ms; j["session_end_utc_ms"] = s.end_utc_ms;
            if (store_->get_media(j["advertisement_id"].get<std::string>(), ma)) j["advertisement_name"] = ma.display_name;
        }
        j["session_status"] = hv ? to_string(ver.status) : "scheduled";
        j["session_status_label"] = hv ? pop_status_label(ver.status) : "Scheduled";
        return j;
    };
    json hits = json::array(); int rank = 0;
    for (const auto& h : res.hits) hits.push_back(hit_json(h, ++rank));
    out["label"] = "Display-evidence similarity (MobileNetV2 embedding of frames stored as Proof-of-Play evidence). Scores are cosine similarities, not "
                   "identification probabilities. Only stored evidence is searched.";
    out["source"] = {{"id", "brightsign_display"}, {"name", "BrightSign Display (Weston/VOP2)"}, {"capture_method", o.capture_method.empty() ? "any" : o.capture_method},
                     {"provenance", "persisted D5 display-capture evidence"}};
    out["query"] = {{"type", "upload"}, {"embedded_with", out["model"].value("name", "")}, {"geometry", geometry},
                    {"geometry_note", geometry == "display"
                        ? "The upload was stretched to 1920x1080, cropped to its top-left 1280x800 and letterboxed into 640x360, the geometry of the stored Weston/VOP2 frames."
                        : "The image was embedded exactly as uploaded. Best scores need display-path frames (640x360 letterbox) or the display geometry option."}};
    if (geometry == "display") {
        static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64; b64.reserve(transformed.size() * 4 / 3 + 4);
        for (size_t i = 0; i < transformed.size(); i += 3) {
            uint32_t n = transformed[i] << 16; if (i + 1 < transformed.size()) n |= transformed[i + 1] << 8; if (i + 2 < transformed.size()) n |= transformed[i + 2];
            b64 += T[(n >> 18) & 63]; b64 += T[(n >> 12) & 63]; b64 += i + 1 < transformed.size() ? T[(n >> 6) & 63] : '='; b64 += i + 2 < transformed.size() ? T[n & 63] : '=';
        }
        out["query"]["uploaded_size"] = std::to_string(up_w) + "x" + std::to_string(up_h);
        out["query"]["embedded_size"] = "640x360";
        out["query"]["embedded_jpeg_b64"] = b64;
    }
    out["options"] = {{"top_k", o.top_k}, {"min_similarity", o.min_similarity}, {"min_similarity_default", o.min_similarity == cfg_.thresholds.match_sim},
                      {"start", o.start_utc_ms}, {"end", o.end_utc_ms}, {"media_id", o.media_id}, {"require_image", o.require_image}, {"session_id", o.session_id}, {"role", o.role.empty() ? "any" : o.role}};
    out["status"] = {{"code", "ok"}, {"message", ""}, {"elapsed_ms", res.elapsed_ms}, {"embed_ms", res.embed_ms}, {"scan_ms", res.scan_ms}, {"candidates_scanned", res.scanned}, {"skipped_invalid", res.skipped_invalid}};
    out["results"] = hits;
    out["coverage"] = {{"evidence_rows", res.coverage.rows_total}, {"searchable_rows", res.coverage.rows_searchable}, {"other_model_rows", res.coverage.rows_other_model},
                       {"images_retained", res.coverage.images_retained}, {"sessions", res.coverage.sessions},
                       {"checkpoint_rows", res.coverage.checkpoint_rows}, {"history_rows", res.coverage.history_rows},
                       {"oldest_utc_ms", res.coverage.oldest_utc_ms ? json(res.coverage.oldest_utc_ms) : json(nullptr)},
                       {"newest_utc_ms", res.coverage.newest_utc_ms ? json(res.coverage.newest_utc_ms) : json(nullptr)}};
    if (hits.empty()) {
        out["no_match"] = true;
        out["message"] = "No matching stored evidence";
        out["message_detail"] = "This does not establish that the content was never displayed. No stored display evidence reached the minimum similarity: it only means "
                                "that none of the " + std::to_string(res.coverage.rows_searchable) + " stored evidence frames matches. Playback outside a registered "
                                "advertisement session is not stored.";
        if (res.has_closest) out["closest_below_minimum"] = hit_json(res.closest, 0);
    }
    http = 200;
    return true;
}

json PopService::session_json(const PopSession& s) {
    Verification v; const bool have = store_->get_verification(s.session_id, v);
    json j = {{"session_id", s.session_id}, {"job_id", s.job_id}, {"expected_media_id", s.expected_media_id}, {"played_media_id", s.played_media_id},
              {"source", s.source}, {"state", s.state}, {"end_reason", s.end_reason}, {"scheduled_utc_ms", s.scheduled_utc_ms},
              {"start_utc_ms", s.start_utc_ms}, {"end_utc_ms", s.end_utc_ms}, {"actual_play_ms", s.actual_play_ms}, {"slot_ms", s.slot_ms},
              {"captures_requested", s.captures_requested}, {"created_utc_ms", s.created_utc_ms}};
    // Which backend captures (or captured) this session's evidence: the scheduled plan while it runs, else what the stored evidence says.
    std::string backend; int ev_n = 0, ev_ok = 0;
    for (const auto& e : store_->list_evidence(s.session_id)) { ++ev_n; if (e.status == "ok") ++ev_ok; if (backend.empty()) backend = e.capture_method; }
    if (backend.empty()) { std::lock_guard<std::mutex> pl(plan_mu_); if (plans_.count(s.session_id)) backend = "resilience_grpc"; }
    j["capture_backend"] = backend; j["evidence_count"] = ev_n; j["evidence_ok"] = ev_ok;
    j["status"] = have ? to_string(v.status) : "scheduled";
    j["status_label"] = have ? pop_status_label(v.status) : "Scheduled";
    j["reason"] = have ? v.reason : "no playback event or screenshot received yet";
    if (have) j["verification"] = {{"frames_ok", v.frames_ok}, {"frames_matched", v.frames_matched}, {"frames_mismatched", v.frames_mismatched},
                                   {"frames_failed", v.frames_failed}, {"mean_sim_expected", v.mean_sim_expected}, {"min_sim_expected", v.min_sim_expected},
                                   {"best_media_id", v.best_media_id}, {"playback_complete", v.playback_complete}, {"created_utc_ms", v.created_utc_ms}};
    return j;
}

json PopService::session_detail(const std::string& id, bool* found) {
    PopSession s;
    if (!store_->get_session(id, s)) { if (found) *found = false; return json(); }
    if (found) *found = true;
    json j = session_json(s);
    json ev = json::array();
    for (const auto& e : store_->list_events(id)) {
        json d = json::parse(e.detail_json, nullptr, false);
        ev.push_back({{"event_id", e.event_id}, {"type", e.type}, {"mono_ms", e.mono_ms}, {"utc_ms", e.utc_ms}, {"detail", d.is_discarded() ? json::object() : d}});
    }
    j["events"] = ev;
    if (store_->history_ready()) {
        uint64_t total = 0; const auto hs = store_->list_history(id, 1000, 0, &total);
        uint64_t emb = 0, imgs = 0, linked = 0; for (const auto& r : hs) { emb += r.embedded; imgs += r.image_available || r.evidence_id; linked += r.evidence_id != 0; }
        j["history"] = {{"frames", total}, {"embedded", emb}, {"images_available", imgs}, {"reused_checkpoint", linked}, {"interval_ms", cfg_.history.interval_ms},
                        {"first_offset_ms", hs.empty() ? json(nullptr) : json(hs.front().actual_offset_ms)}, {"last_offset_ms", hs.empty() ? json(nullptr) : json(hs.back().actual_offset_ms)},
                        {"frames_url", "/api/pop/history/frames?session_id=" + id}};
    }
    json evd = json::array();
    for (const auto& e : store_->list_evidence(id)) evd.push_back(evidence_json(e));
    j["evidence"] = evd;
    const std::string vj = store_->get_verification_json(id);
    if (!vj.empty()) j["verification_detail"] = json::parse(vj, nullptr, false);
    MediaAsset m;
    if (store_->get_media(s.expected_media_id, m)) j["expected_media"] = media_json(m);
    if (!s.played_media_id.empty() && s.played_media_id != s.expected_media_id && store_->get_media(s.played_media_id, m)) j["played_media"] = media_json(m);
    // reference frames of the expected media (for the side-by-side comparison)
    json refs = json::array();
    for (const auto& r : store_->list_references(s.expected_media_id))
        refs.push_back({{"ref_id", r.ref_id}, {"offset_ms", r.offset_ms}, {"image_url", "/api/pop/media/" + s.expected_media_id + "/references/" + std::to_string(r.ref_id) + "/image"}});
    j["reference_frames"] = refs;
    return j;
}

std::string PopService::export_csv(const SessionFilter& f) {
    std::string o = "session_id,expected_media_id,played_media_id,status,reason,scheduled_utc_ms,start_utc_ms,end_utc_ms,actual_play_ms,slot_ms,"
                    "end_reason,frames_ok,frames_matched,frames_mismatched,frames_failed,mean_sim_expected,min_sim_expected\n";
    for (const auto& s : store_->list_sessions(f, 5000, 0)) {
        Verification v; const bool have = store_->get_verification(s.session_id, v);
        char buf[64];
        o += csv_esc(s.session_id) + "," + csv_esc(s.expected_media_id) + "," + csv_esc(s.played_media_id) + "," + (have ? to_string(v.status) : "scheduled") + "," +
             csv_esc(have ? v.reason : "") + "," + std::to_string(s.scheduled_utc_ms) + "," + std::to_string(s.start_utc_ms) + "," + std::to_string(s.end_utc_ms) + "," +
             std::to_string(s.actual_play_ms) + "," + std::to_string(s.slot_ms) + "," + csv_esc(s.end_reason) + "," +
             std::to_string(have ? v.frames_ok : 0) + "," + std::to_string(have ? v.frames_matched : 0) + "," + std::to_string(have ? v.frames_mismatched : 0) + "," +
             std::to_string(have ? v.frames_failed : 0) + ",";
        std::snprintf(buf, sizeof(buf), "%.4f,%.4f", have ? v.mean_sim_expected : 0.0, have ? v.min_sim_expected : 0.0);
        o += std::string(buf) + "\n";
    }
    return o;
}

void PopService::prune() {
    const int64_t cutoff = cfg_.evidence_retention_days > 0 ? now_ms() - static_cast<int64_t>(cfg_.evidence_retention_days) * 86400000LL : 0;
    store_->prune_evidence(cutoff, cfg_.evidence_max_bytes);
    const auto& h = cfg_.history;
    if (store_->history_ready())                       // history quota and retention are independent of the checkpoint evidence limits
        store_->prune_history(h.jpeg_retention_days > 0 ? now_ms() - static_cast<int64_t>(h.jpeg_retention_days) * 86400000LL : 0, h.max_bytes,
                              h.metadata_retention_days > 0 ? now_ms() - static_cast<int64_t>(h.metadata_retention_days) * 86400000LL : 0, h.max_rows);
}

json PopService::status() {
    const PopSummary s = store_->summary();
    std::lock_guard<std::mutex> lk(mu_);
    json th = {{"match_sim", cfg_.thresholds.match_sim}, {"reject_sim", cfg_.thresholds.reject_sim}, {"margin", cfg_.thresholds.margin},
               {"min_play_ratio", cfg_.thresholds.min_play_ratio}, {"min_match_fraction", cfg_.thresholds.min_match_fraction}};
    return {{"enabled", cfg_.enabled}, {"store", {{"open", s.open}, {"schema_version", s.schema_version}, {"media", s.media}, {"reference_frames", s.reference_frames},
            {"sessions", s.sessions}, {"events", s.events}, {"evidence", s.evidence}, {"evidence_bytes", s.evidence_bytes}, {"verifications", s.verifications},
            {"db_bytes", s.db_bytes}}},
            {"adapter", {{"running", running_.load()}, {"events_path", cfg_.events_path}, {"file_bytes", file_size_}, {"offset", offset_}, {"lines", lines_},
                         {"events_ingested", events_}, {"errors", errors_}, {"boots", boots_}, {"last_event_utc_ms", last_event_utc_ms_}, {"last_error", last_error_}}},
            {"capture_backend", cfg_.capture_backend}, {"schema_legacy_v1", !store_->schema_v2()}, {"display_source_available", display_ != nullptr && display_->cfg().enabled},
            {"allow_test_jobs", cfg_.allow_test_jobs}, {"thresholds", th}, {"evidence_max_bytes", cfg_.evidence_max_bytes},
            {"evidence_retention_days", cfg_.evidence_retention_days}, {"source", cfg_.source_id},
            {"evidence_source", "presentation output of the player (display capture); not the audience camera"},
            {"history", {{"enabled", cfg_.history.enabled}, {"ready", store_->history_ready()}}}};
}


// Read-only system snapshot for performance validation: counters are cumulative, the client computes rates from two samples.
json PopService::system_json() {
    json out = {{"utc_ms", now_ms()}};
    auto slurp = [](const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); };
    {   // system CPU jiffies
        std::istringstream is(slurp("/proc/stat")); std::string tag; is >> tag;
        long long v[10] = {0}; for (int i = 0; i < 10 && (is >> v[i]); ++i) {}
        long long total = 0; for (long long x : v) total += x;
        out["cpu"] = {{"total_jiffies", total}, {"idle_jiffies", v[3] + v[4]}, {"cores", static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN))}, {"hz", sysconf(_SC_CLK_TCK)}};
    }
    {   // memory
        std::istringstream is(slurp("/proc/meminfo")); std::string k, unit; long long kb; json m = json::object();
        while (is >> k >> kb >> unit) if (k == "MemTotal:" || k == "MemAvailable:" || k == "MemFree:") m[k.substr(0, k.size() - 1)] = kb;
        out["mem_kb"] = m;
    }
    out["loadavg"] = slurp("/proc/loadavg").substr(0, 24);
    {   // block devices (SD card / USB): cumulative sectors (512 B) read and written, for SD write-rate measurements
        json disks = json::object(); std::error_code ec;
        for (auto it = fs::directory_iterator("/sys/block", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (n.rfind("mmcblk", 0) != 0 && n.rfind("sd", 0) != 0) continue;
            std::istringstream is(slurp("/sys/block/" + n + "/stat")); long long v[11] = {0}; for (int i = 0; i < 11 && (is >> v[i]); ++i) {}
            disks[n] = {{"sectors_read", v[2]}, {"sectors_written", v[6]}};
        }
        out["disks"] = disks;
    }
    {   // per-process counters for the processes that matter to the audience pipeline and to D5
        static const char* names[] = {"attention_demo", "image-stream-se", "dashboard-serve", "vm_api_server", "argus-exporter", "mosquitto", "bs-resilience-ap", "BrightSign", "argus_display_b", "bs-rendered-fra", "weston"};
        json procs = json::array();
        std::error_code ec;
        for (auto it = fs::directory_iterator("/proc", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string pid = it->path().filename().string();
            if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
            const std::string st = slurp("/proc/" + pid + "/stat");
            const size_t a = st.find('('), b = st.rfind(')');
            if (a == std::string::npos || b == std::string::npos) continue;
            const std::string comm = st.substr(a + 1, b - a - 1);
            bool want = false; for (const char* n : names) if (comm.rfind(n, 0) == 0) want = true;
            if (!want) continue;
            std::istringstream rest(st.substr(b + 2)); std::string f; std::vector<std::string> v;
            while (rest >> f) v.push_back(f);
            if (v.size() < 22) continue;                                      // fields after ')' start at index 0 = state (field 3)
            const long long ut = std::atoll(v[11].c_str()), stt = std::atoll(v[12].c_str()), rss_pages = std::atoll(v[21].c_str());
            long long hwm = 0;
            { std::istringstream sis(slurp("/proc/" + pid + "/status")); std::string k, rest2; while (std::getline(sis, k)) if (k.rfind("VmHWM:", 0) == 0) hwm = std::atoll(k.c_str() + 6); }
            long long wbytes = -1, rbytes = -1;                                  // cumulative storage I/O of the process (needs the same uid; -1 when unreadable)
            { std::istringstream iis(slurp("/proc/" + pid + "/io")); std::string k; while (std::getline(iis, k)) { if (k.rfind("write_bytes:", 0) == 0) wbytes = std::atoll(k.c_str() + 12); else if (k.rfind("read_bytes:", 0) == 0) rbytes = std::atoll(k.c_str() + 11); } }
            procs.push_back({{"pid", std::atoi(pid.c_str())}, {"comm", comm}, {"cpu_jiffies", ut + stt}, {"rss_kb", rss_pages * (sysconf(_SC_PAGESIZE) / 1024)}, {"hwm_kb", hwm},
                             {"io_write_bytes", wbytes}, {"io_read_bytes", rbytes}});
        }
        out["processes"] = procs;
    }
    for (const char* p : {"/sys/kernel/debug/rknpu/load", "/sys/class/devfreq/fdab0000.npu/load"}) {
        const std::string t = slurp(p);
        if (!t.empty()) { out["npu_load"] = t.substr(0, 160); break; }
    }
    {
        json temps = json::array();
        std::error_code ec;
        for (auto it = fs::directory_iterator("/sys/class/thermal", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (n.rfind("thermal_zone", 0) != 0) continue;
            const std::string tp = slurp(it->path().string() + "/temp"), ty = slurp(it->path().string() + "/type");
            if (!tp.empty()) temps.push_back({{"zone", ty.substr(0, ty.find('\n'))}, {"milli_c", std::atoll(tp.c_str())}});
        }
        out["thermal"] = temps;
    }
    return out;
}

} // namespace argus_pop
