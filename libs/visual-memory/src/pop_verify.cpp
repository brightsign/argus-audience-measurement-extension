// SPDX-License-Identifier: MIT
// D5: perceptual hash and the pure verification decision logic (no I/O).
#include "visualmemory/pop.h"

#include "jpeg_decoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace visualmemory {

namespace {
const char* kEngineVersion = "pop-verify-1";

float dot(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return -1.0f;
    double s = 0;
    for (size_t i = 0; i < a.size(); ++i) s += static_cast<double>(a[i]) * b[i];
    return static_cast<float>(s);
}

std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; } else o += static_cast<char>(c);
        }
    }
    return o;
}
std::string num(double v) {
    if (!std::isfinite(v)) return "null";
    char b[40]; std::snprintf(b, sizeof(b), "%.4f", v); return b;
}
} // namespace

// ---- status names -----------------------------------------------------------------------------------------------
const char* to_string(PopStatus s) {
    switch (s) {
        case PopStatus::Scheduled: return "scheduled";
        case PopStatus::PlaybackReported: return "playback_reported";
        case PopStatus::VisualMatch: return "visual_match";
        case PopStatus::PlaybackCorroborated: return "playback_corroborated";
        case PopStatus::Mismatch: return "mismatch";
        case PopStatus::Incomplete: return "incomplete";
        case PopStatus::CaptureUnsupported: return "capture_unsupported";
    }
    return "scheduled";
}
const char* pop_status_label(PopStatus s) {
    switch (s) {
        case PopStatus::Scheduled: return "Scheduled";
        case PopStatus::PlaybackReported: return "Playback reported";
        case PopStatus::VisualMatch: return "Visual match";
        case PopStatus::PlaybackCorroborated: return "Playback corroborated";
        case PopStatus::Mismatch: return "Mismatch";
        case PopStatus::Incomplete: return "Incomplete";
        case PopStatus::CaptureUnsupported: return "Capture unsupported";
    }
    return "Scheduled";
}
bool parse_pop_status(const std::string& s, PopStatus& out) {
    static const PopStatus all[] = {PopStatus::Scheduled, PopStatus::PlaybackReported, PopStatus::VisualMatch,
                                    PopStatus::PlaybackCorroborated, PopStatus::Mismatch, PopStatus::Incomplete,
                                    PopStatus::CaptureUnsupported};
    for (PopStatus p : all) if (s == to_string(p)) { out = p; return true; }
    return false;
}

// ---- perceptual hash --------------------------------------------------------------------------------------------
int phash_distance(uint64_t a, uint64_t b) { return __builtin_popcountll(a ^ b); }

bool phash_of_jpeg(const std::vector<uint8_t>& jpeg, uint64_t& out, std::string& err) {
    detail::DecodedImage img;
    if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) return false;
    constexpr int N = 32;
    // area-average to 32x32 grey (aspect deliberately ignored: references and captures differ in aspect ratio)
    double g[N][N];
    for (int y = 0; y < N; ++y) {
        const int y0 = y * img.height / N, y1 = std::max(y0 + 1, (y + 1) * img.height / N);
        for (int x = 0; x < N; ++x) {
            const int x0 = x * img.width / N, x1 = std::max(x0 + 1, (x + 1) * img.width / N);
            double acc = 0; int n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* p = &img.rgb[(static_cast<size_t>(yy) * img.width + xx) * 3];
                    acc += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; ++n;
                }
            g[y][x] = n ? acc / n : 0;
        }
    }
    static double c[8][N]; static bool init = false;
    if (!init) {
        for (int u = 0; u < 8; ++u) for (int i = 0; i < N; ++i) c[u][i] = std::cos((2 * i + 1) * u * M_PI / (2.0 * N));
        init = true;
    }
    double d[8][8];
    for (int u = 0; u < 8; ++u)
        for (int v = 0; v < 8; ++v) {
            double s = 0;
            for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) s += g[y][x] * c[v][y] * c[u][x];
            d[v][u] = s;
        }
    std::vector<double> ac;
    for (int v = 0; v < 8; ++v) for (int u = 0; u < 8; ++u) if (u || v) ac.push_back(d[v][u]);
    std::vector<double> sorted = ac; std::sort(sorted.begin(), sorted.end());
    const double med = sorted[sorted.size() / 2];
    uint64_t h = 0; int bit = 0;
    for (int v = 0; v < 8; ++v) for (int u = 0; u < 8; ++u) {
        if (!u && !v) { ++bit; continue; }                       // DC term carries brightness only
        if (d[v][u] > med) h |= (1ULL << bit);
        ++bit;
    }
    out = h;
    return true;
}

// ---- decision logic ---------------------------------------------------------------------------------------------
Verification verify_playback(const VerifyInput& in, const VerifyThresholds& t) {
    Verification v;
    v.expected_media_id = in.expected_media_id;
    v.engine_version = kEngineVersion;
    v.slot_ms = in.slot_ms;
    v.actual_play_ms = in.actual_play_ms;
    v.playback_reported = in.reported;
    v.session_ended = in.ended;
    v.frames_failed = in.captures_failed;
    v.frames_total = static_cast<int>(in.evidence.size()) + in.captures_failed;
    v.frames_ok = static_cast<int>(in.evidence.size());
    v.created_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    const bool reason_ok = in.end_reason == "slot_end" || in.end_reason == "ended";
    v.playback_complete = in.reported && in.ended && reason_ok && in.actual_play_ms >= 0 &&
                          static_cast<double>(in.actual_play_ms) >= t.min_play_ratio * static_cast<double>(in.slot_ms);
    const bool interrupted = in.reported && in.ended && !v.playback_complete;

    // ---- per-frame scores ----
    double sum_exp = 0, sum_ph = 0; float min_exp = 2.0f; int ph_n = 0;
    std::vector<std::pair<std::string, int>> best_votes;
    for (const auto& e : in.evidence) {
        FrameScore f; f.idx = e.idx; f.evidence_id = e.evidence_id; f.actual_offset_ms = e.actual_offset_ms;
        float other_best = -1.0f; std::string other_media; int other_off = -1;
        for (const auto& r : in.refs) {
            const float s = dot(e.emb, r.emb);
            if (r.media_id == in.expected_media_id) {
                if (s > f.sim_expected) { f.sim_expected = s; f.best_ref_offset_ms = r.offset_ms; }
                f.phash_dist_expected = std::min(f.phash_dist_expected, phash_distance(e.phash, r.phash));
            } else if (s > other_best) { other_best = s; other_media = r.media_id; other_off = r.offset_ms; }
        }
        f.sim_other = other_best; f.best_other_media = other_media; f.best_other_ref_offset_ms = other_off;
        const bool have_exp = f.sim_expected >= 0.0f || f.best_ref_offset_ms >= 0;
        if (!have_exp) f.verdict = "inconclusive";                                   // no reference frames of the expected media
        else if (f.sim_expected >= t.match_sim && !(other_best >= t.match_sim && other_best > f.sim_expected + t.margin)) f.verdict = "match";
        else if ((other_best >= t.match_sim && other_best > f.sim_expected + t.margin) || f.sim_expected < t.reject_sim) f.verdict = "mismatch";
        else f.verdict = "inconclusive";
        if (f.verdict == "match") ++v.frames_matched;
        else if (f.verdict == "mismatch") ++v.frames_mismatched;
        sum_exp += f.sim_expected; min_exp = std::min(min_exp, f.sim_expected);
        sum_ph += f.phash_dist_expected; ++ph_n;
        if (f.verdict == "mismatch" && !other_media.empty() && other_best > f.sim_expected) {
            bool found = false;
            for (auto& bv : best_votes) if (bv.first == other_media) { ++bv.second; found = true; }
            if (!found) best_votes.push_back({other_media, 1});
        }
        v.frames.push_back(std::move(f));
    }
    if (v.frames_ok > 0) { v.mean_sim_expected = sum_exp / v.frames_ok; v.min_sim_expected = min_exp; v.mean_phash_dist = ph_n ? sum_ph / ph_n : 0; }
    v.best_media_id = in.expected_media_id;
    if (!best_votes.empty() && v.frames_mismatched > 0) {
        auto it = std::max_element(best_votes.begin(), best_votes.end(), [](auto& a, auto& b) { return a.second < b.second; });
        v.best_media_id = it->first;
    }

    char buf[256];
    const double mfrac = v.frames_ok ? static_cast<double>(v.frames_matched) / v.frames_ok : 0.0;
    const bool content_mismatch = v.frames_ok > 0 && v.frames_mismatched > 0 &&
                                  static_cast<double>(v.frames_mismatched) / v.frames_ok > 0.5 && mfrac < t.min_match_fraction;

    if (!in.reported && v.frames_ok == 0 && in.captures_failed == 0) {
        v.status = PopStatus::Scheduled; v.reason = "no playback event or screenshot received yet";
    } else if (content_mismatch) {
        v.status = PopStatus::Mismatch;
        std::snprintf(buf, sizeof(buf), "%d of %d screenshots do not resemble '%s' (mean similarity %.3f)%s%s",
                      v.frames_mismatched, v.frames_ok, in.expected_media_id.c_str(), v.mean_sim_expected,
                      v.best_media_id != in.expected_media_id ? "; closest registered media: " : "",
                      v.best_media_id != in.expected_media_id ? v.best_media_id.c_str() : "");
        v.reason = buf;
    } else if (interrupted) {
        v.status = PopStatus::Incomplete;
        std::snprintf(buf, sizeof(buf), "playback ended early (%s) after %lld ms of a %lld ms slot",
                      in.end_reason.empty() ? "no reason reported" : in.end_reason.c_str(),
                      static_cast<long long>(in.actual_play_ms < 0 ? 0 : in.actual_play_ms), static_cast<long long>(in.slot_ms));
        v.reason = buf;
    } else if (v.frames_ok == 0) {
        if (in.reported && in.captures_failed > 0) {
            v.status = PopStatus::CaptureUnsupported;
            v.reason = "playback was reported but no screenshot of the display could be obtained: visual verification unavailable";
        } else if (in.reported) {
            v.status = PopStatus::PlaybackReported; v.reason = "playback reported; no screenshot evidence";
        } else {
            v.status = PopStatus::Scheduled; v.reason = "waiting for playback";
        }
    } else if (v.frames_matched > 0 && mfrac >= t.min_match_fraction) {
        if (v.playback_complete) {
            v.status = PopStatus::PlaybackCorroborated;
            std::snprintf(buf, sizeof(buf), "playback events complete (%lld ms of %lld ms) and %d of %d screenshots match '%s' (mean similarity %.3f)",
                          static_cast<long long>(in.actual_play_ms), static_cast<long long>(in.slot_ms), v.frames_matched, v.frames_ok,
                          in.expected_media_id.c_str(), v.mean_sim_expected);
        } else {
            v.status = PopStatus::VisualMatch;
            std::snprintf(buf, sizeof(buf), "%d of %d screenshots match '%s' (mean similarity %.3f) but the player's events do not yet show a complete play",
                          v.frames_matched, v.frames_ok, in.expected_media_id.c_str(), v.mean_sim_expected);
        }
        v.reason = buf;
    } else {
        v.status = PopStatus::PlaybackReported;
        std::snprintf(buf, sizeof(buf), "playback reported; screenshots inconclusive (%d match, %d mismatch of %d)", v.frames_matched, v.frames_mismatched, v.frames_ok);
        v.reason = buf;
    }
    if (in.captures_failed > 0 && v.frames_ok > 0) v.reason += "; " + std::to_string(in.captures_failed) + " screenshot(s) failed";
    return v;
}

std::string verification_to_json(const Verification& v) {
    std::string o = "{";
    o += "\"session_id\":\"" + esc(v.session_id) + "\",\"status\":\"" + to_string(v.status) + "\",\"label\":\"" + pop_status_label(v.status) +
         "\",\"reason\":\"" + esc(v.reason) + "\",\"expected_media_id\":\"" + esc(v.expected_media_id) + "\",\"best_media_id\":\"" + esc(v.best_media_id) + "\"";
    o += ",\"frames_total\":" + std::to_string(v.frames_total) + ",\"frames_ok\":" + std::to_string(v.frames_ok) +
         ",\"frames_matched\":" + std::to_string(v.frames_matched) + ",\"frames_mismatched\":" + std::to_string(v.frames_mismatched) +
         ",\"frames_failed\":" + std::to_string(v.frames_failed);
    o += ",\"mean_sim_expected\":" + num(v.mean_sim_expected) + ",\"min_sim_expected\":" + num(v.min_sim_expected) + ",\"mean_phash_dist\":" + num(v.mean_phash_dist);
    o += std::string(",\"playback_reported\":") + (v.playback_reported ? "true" : "false") + ",\"playback_complete\":" + (v.playback_complete ? "true" : "false") +
         ",\"session_ended\":" + (v.session_ended ? "true" : "false") + ",\"actual_play_ms\":" + std::to_string(v.actual_play_ms) + ",\"slot_ms\":" + std::to_string(v.slot_ms);
    o += ",\"engine_version\":\"" + esc(v.engine_version) + "\",\"created_utc_ms\":" + std::to_string(v.created_utc_ms) + ",\"frames\":[";
    for (size_t i = 0; i < v.frames.size(); ++i) {
        const auto& f = v.frames[i];
        if (i) o += ",";
        o += "{\"idx\":" + std::to_string(f.idx) + ",\"evidence_id\":" + std::to_string(f.evidence_id) + ",\"actual_offset_ms\":" + std::to_string(f.actual_offset_ms) +
             ",\"sim_expected\":" + num(f.sim_expected) + ",\"best_ref_offset_ms\":" + std::to_string(f.best_ref_offset_ms) +
             ",\"phash_dist_expected\":" + std::to_string(f.phash_dist_expected) + ",\"sim_other\":" + num(f.sim_other) +
             ",\"best_other_media\":\"" + esc(f.best_other_media) + "\",\"best_other_ref_offset_ms\":" + std::to_string(f.best_other_ref_offset_ms) + ",\"verdict\":\"" + f.verdict + "\"}";
    }
    return o + "]}";
}

} // namespace visualmemory
