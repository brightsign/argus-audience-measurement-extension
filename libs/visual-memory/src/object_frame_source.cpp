// SPDX-License-Identifier: MIT
#include "visualmemory/object_frame_source.h"

#include "visualmemory/pop.h"
#include "visualmemory/reader.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <filesystem>
#include <map>

namespace visualmemory {

// ------------------------------------------------------------------------------------------------ camera
bool CameraObjectSource::ensure_open(std::string& err) {
    if (r_.is_open()) return true;
    return r_.open(err);
}

namespace {
ObjSrcFrame from_camera(const FrameRecord& fr) {
    ObjSrcFrame f;
    f.source_type = "camera"; f.kind.clear(); f.id = fr.frame.id; f.source_id = fr.frame.metadata.source_id;
    f.capture_utc_ms = fr.frame.metadata.capture_utc_ms; f.width = fr.frame.width; f.height = fr.frame.height;
    f.image_available = fr.frame.image_available; f.image_exists = fr.image_exists; f.sha256 = fr.frame.sha256_hex;
    return f;
}
} // namespace

bool CameraObjectSource::get(const std::string&, int64_t id, ObjSrcFrame& out) {
    FrameDetail fd;
    if (!r_.get_frame(id, fd)) return false;
    out = from_camera(fd.record);
    return true;
}
int CameraObjectSource::read_jpeg(const std::string&, int64_t id, std::vector<uint8_t>& out, std::string& err) { return r_.read_jpeg(id, out, err); }

std::vector<ObjSrcFrame> CameraObjectSource::list_after(int64_t after_utc_ms, int limit, uint64_t* total) {
    FrameFilter ff; ff.start_utc_ms = after_utc_ms + 1;
    std::vector<ObjSrcFrame> v;
    for (const auto& fr : r_.list_frames(ff, limit, 0, /*newest_first=*/true, total)) v.push_back(from_camera(fr));
    return v;
}
std::vector<ObjSrcFrame> CameraObjectSource::list_range(int64_t start, int64_t end, int limit, bool newest_first) {
    FrameFilter ff; ff.start_utc_ms = start; ff.end_utc_ms = end;
    std::vector<ObjSrcFrame> v;
    for (const auto& fr : r_.list_frames(ff, limit, 0, newest_first, nullptr)) v.push_back(from_camera(fr));
    return v;
}
void CameraObjectSource::counts(uint64_t& stored, uint64_t& with_image) {
    const StoreSummary s = r_.summary();
    stored = s.frames; with_image = s.frames_with_image;
}

// ------------------------------------------------------------------------------------------------ display (Proof of Play)
namespace {
constexpr const char* kWeston = "resilience_grpc";          // Weston/VOP2 writeback through the OS rendered-frame service

// Session facts (expected / player-reported advertisement) are looked up once per session.
struct SessionNames { std::string expected, reported; };
SessionNames session_names(PopStore& s, const std::string& sid, std::map<std::string, SessionNames>& cache) {
    auto it = cache.find(sid);
    if (it != cache.end()) return it->second;
    SessionNames n; PopSession ps;
    if (!sid.empty() && s.get_session(sid, ps)) { n.expected = ps.expected_media_id; n.reported = ps.played_media_id; }
    return cache[sid] = n;
}
ObjSrcFrame from_ref(PopStore& s, const DisplayFrameRef& r, std::map<std::string, SessionNames>& cache) {
    ObjSrcFrame f;
    f.source_type = "display"; f.kind = r.frame_id ? "history" : "checkpoint"; f.id = r.frame_id ? r.frame_id : r.evidence_id;
    f.source_id = r.source; f.capture_utc_ms = r.utc_ms; f.width = r.width; f.height = r.height;
    f.image_available = r.image_available; f.image_exists = r.image_available; f.sha256 = r.sha256;
    f.session_id = r.session_id; f.capture_method = r.capture_method; f.provenance = r.provenance;
    f.playback_offset_ms = r.actual_offset_ms; f.time_uncertainty_ms = r.uncertainty_ms;
    const SessionNames n = session_names(s, r.session_id, cache);
    f.expected_media_id = !r.expected_media_id.empty() ? r.expected_media_id : n.expected; f.reported_media_id = n.reported;
    return f;
}
} // namespace

bool DisplayObjectSource::ensure_open(std::string& err) {
    if (s_.is_open()) return true;
    err = "the Proof-of-Play store is not open";
    return false;
}

bool DisplayObjectSource::get(const std::string& kind, int64_t id, ObjSrcFrame& out) {
    std::map<std::string, SessionNames> cache;
    DisplayFrameRef r; std::string rel;                          // rel: JPEG path inside the Proof-of-Play root
    if (kind == "history") {
        HistoryFrame h;
        if (!s_.get_history(id, h) || h.capture_method != kWeston) return false;
        r.frame_id = h.frame_id; r.evidence_id = h.evidence_id; r.session_id = h.session_id; r.source = h.source; r.capture_method = h.capture_method;
        r.provenance = h.provenance; r.expected_media_id = h.expected_media_id; r.utc_ms = h.utc_ms; r.actual_offset_ms = h.actual_offset_ms;
        r.uncertainty_ms = h.uncertainty_ms; r.width = h.width; r.height = h.height; r.bytes = h.bytes; r.sha256 = h.sha256;
        r.embedded = h.embedded; r.image_available = h.image_available; rel = h.path;
        if (h.evidence_id) {                                     // a history instant that is also a verification checkpoint stores NO second JPEG: its picture is the checkpoint's
            Evidence e;
            if (s_.get_evidence(h.evidence_id, e)) { rel = e.path; r.image_available = e.image_available; }
            else { rel.clear(); r.image_available = false; }
        }
    } else if (kind == "checkpoint") {
        Evidence e;
        if (!s_.get_evidence(id, e) || e.capture_method != kWeston || e.status != "ok") return false;
        r.evidence_id = e.evidence_id; r.session_id = e.session_id; r.source = e.source; r.capture_method = e.capture_method; r.provenance = e.provenance;
        r.expected_media_id = e.expected_media_id; r.utc_ms = e.utc_ms; r.actual_offset_ms = e.actual_offset_ms; r.uncertainty_ms = e.uncertainty_ms;
        r.width = e.width; r.height = e.height; r.bytes = e.bytes; r.sha256 = e.sha256; r.embedded = e.embedded; r.image_available = e.image_available; rel = e.path;
    } else return false;
    out = from_ref(s_, r, cache);
    if (r.image_available && !rel.empty()) {                     // the flag says "retained"; also check the file really is there (a manual delete, a bad card)
        std::error_code ec; out.image_exists = std::filesystem::exists(std::filesystem::path(s_.root()) / rel, ec);
    }
    out.kind = kind; out.id = id;                              // the identity the caller asked for (a checkpoint linked to history stays addressable)
    return true;
}

int DisplayObjectSource::read_jpeg(const std::string& kind, int64_t id, std::vector<uint8_t>& out, std::string& err) {
    ObjSrcFrame f;
    if (!get(kind, id, f)) { err = "unknown display frame"; return 1; }
    if (!f.image_available) { err = "the display frame JPEG was removed by retention"; return 2; }
    const bool ok = kind == "history" ? s_.history_jpeg(id, out) : s_.evidence_jpeg(id, out);
    if (!ok) { err = "the display frame JPEG is missing or unreadable"; return 3; }
    return 0;
}

std::vector<ObjSrcFrame> DisplayObjectSource::list_after(int64_t after_utc_ms, int limit, uint64_t* total) {
    bool trunc = false;
    const auto refs = s_.display_frames_between(after_utc_ms + 1, std::numeric_limits<int64_t>::max() / 4, "", 5000, &trunc, /*newest_first=*/true);
    std::map<std::string, SessionNames> cache; std::vector<ObjSrcFrame> v;
    for (const auto& r : refs) { if (static_cast<int>(v.size()) >= limit) break; v.push_back(from_ref(s_, r, cache)); }
    if (total) *total = refs.size();
    return v;
}
std::vector<ObjSrcFrame> DisplayObjectSource::list_range(int64_t start, int64_t end, int limit, bool newest_first) {
    bool trunc = false;
    const int64_t to = end > 0 ? end - 1 : std::numeric_limits<int64_t>::max() / 4;
    const auto refs = s_.display_frames_between(start, to, "", std::min(std::max(limit, 1), 5000), &trunc, newest_first);
    std::map<std::string, SessionNames> cache; std::vector<ObjSrcFrame> v;
    for (const auto& r : refs) v.push_back(from_ref(s_, r, cache));
    return v;
}
void DisplayObjectSource::counts(uint64_t& stored, uint64_t& with_image) {
    bool trunc = false;
    const auto refs = s_.display_frames_between(0, std::numeric_limits<int64_t>::max() / 4, "", 5000, &trunc, true);   // bounded (newest 5000)
    stored = refs.size(); with_image = 0;
    for (const auto& r : refs) if (r.image_available) ++with_image;
}

} // namespace visualmemory
