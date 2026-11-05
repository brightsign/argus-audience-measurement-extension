// SPDX-License-Identifier: MIT
#include "pop_service.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
            {"image_available", e.image_available},
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
    PopConfig pc; pc.root_directory = root_;
    store_ = std::make_unique<PopStore>(pc);
}
PopService::~PopService() { stop(); }

bool PopService::start(std::string& err) {
    if (!store_->open(err)) return false;
    load_state();
    if (cfg_.enabled && !running_.load()) {
        stop_ = false; running_ = true;
        th_ = std::thread([this] { loop(); });
    }
    return true;
}
void PopService::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
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
    e.detail_json = line.size() <= 4000 ? line : std::string("{}");
    std::string err;
    if (!store_->apply_event(e, err)) { std::lock_guard<std::mutex> lk(mu_); ++errors_; last_error_ = "apply_event: " + err; return; }
    { std::lock_guard<std::mutex> lk(mu_); ++events_; last_event_utc_ms_ = utc; }
    if (ev == "session_end") {
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
    if (b.contains("capture_offsets_s")) {
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
    PopSession s; s.session_id = sid; s.job_id = "job-" + sid; s.expected_media_id = expected; s.slot_ms = static_cast<int64_t>(slot * 1000);
    s.captures_requested = static_cast<int>(offs.size()); s.source = cfg_.source_id; s.scheduled_utc_ms = now_ms();
    if (!store_->schedule_session(s, err)) { http = err.find("exists") != std::string::npos ? 409 : 400; return false; }

    json job = {{"job_id", s.job_id}, {"session_id", sid}, {"media_id", play}, {"expected_media_id", expected}, {"media_file", pm.uri},
                {"max_play_ms", s.slot_ms}, {"capture_offsets_ms", offs}, {"capture_width", 640}, {"capture_height", 400},
                {"loop", pm.kind == "file" && pm.duration_s > 0 && pm.duration_s * 1000 < static_cast<double>(s.slot_ms)}};
    if (interrupt >= 0) job["interrupt_at_ms"] = static_cast<int64_t>(interrupt * 1000);
    if (b.value("break_capture", false)) job["capture_dir"] = "SD:/pop/no_such_dir";        // a real, failing Screenshot() call
    char name[96]; std::snprintf(name, sizeof(name), "%013lld_%s.json", static_cast<long long>(now_ms()), sid.c_str());
    if (!write_text_atomic(fs::path(cfg_.jobs_dir) / name, job.dump())) { err = "cannot write the job file into " + cfg_.jobs_dir; http = 503; return false; }
    http = 201;
    out = {{"session_id", sid}, {"job_file", name}, {"expected_media_id", expected}, {"play_media_id", play}, {"slot_ms", s.slot_ms},
           {"capture_offsets_ms", offs}, {"status", "scheduled"}};
    return true;
}

bool PopService::build_references(const std::string& media_id, json& out, int& http, std::string& err, bool replace) {
    http = 400;
    MediaAsset m;
    if (!store_->get_media(media_id, m)) { err = "unknown media_id"; http = 404; return false; }
    const fs::path dir = fs::path(root_) / "pop" / "refs_incoming" / media_id;
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
    if (replace) store_->clear_references(media_id);                  // live sources: refresh, do not accumulate stale scenes
    int added = 0, failed = 0; json errors = json::array();
    std::string why;
    const bool have = run_ && run_([&](EmbeddingProvider& p) {
        for (const auto& f : files) {
            std::vector<uint8_t> bytes; std::string e;
            if (!read_file(f.second.string(), bytes) || !store_->add_reference(p, media_id, f.first, bytes, e)) {
                ++failed; if (errors.size() < 5) errors.push_back(f.second.filename().string() + ": " + e);
            } else { ++added; std::error_code rc; fs::remove(f.second, rc); }
        }
    }, why);
    if (!have) { err = "embedding provider unavailable: " + why; http = 503; return false; }
    MediaAsset after; store_->get_media(media_id, after);
    out = {{"media_id", media_id}, {"added", added}, {"failed", failed}, {"errors", errors}, {"reference_frames", after.reference_frames}};
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

json PopService::session_json(const PopSession& s) {
    Verification v; const bool have = store_->get_verification(s.session_id, v);
    json j = {{"session_id", s.session_id}, {"job_id", s.job_id}, {"expected_media_id", s.expected_media_id}, {"played_media_id", s.played_media_id},
              {"source", s.source}, {"state", s.state}, {"end_reason", s.end_reason}, {"scheduled_utc_ms", s.scheduled_utc_ms},
              {"start_utc_ms", s.start_utc_ms}, {"end_utc_ms", s.end_utc_ms}, {"actual_play_ms", s.actual_play_ms}, {"slot_ms", s.slot_ms},
              {"captures_requested", s.captures_requested}, {"created_utc_ms", s.created_utc_ms}};
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
            {"allow_test_jobs", cfg_.allow_test_jobs}, {"thresholds", th}, {"evidence_max_bytes", cfg_.evidence_max_bytes},
            {"evidence_retention_days", cfg_.evidence_retention_days}, {"source", cfg_.source_id},
            {"evidence_source", "presentation output of the player (display capture); not the audience camera"}};
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
    {   // per-process counters for the processes that matter to the audience pipeline and to D5
        static const char* names[] = {"attention_demo", "image-stream-se", "dashboard-serve", "vm_api_server", "argus-exporter", "mosquitto", "bs-resilience-ap", "BrightSign"};
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
            procs.push_back({{"pid", std::atoi(pid.c_str())}, {"comm", comm}, {"cpu_jiffies", ut + stt}, {"rss_kb", rss_pages * (sysconf(_SC_PAGESIZE) / 1024)}, {"hwm_kb", hwm}});
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
