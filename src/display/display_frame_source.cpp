// SPDX-License-Identifier: MIT
#include "display/display_frame_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace argus_display {

using json = nlohmann::json;

int64_t mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t utc_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool jpeg_info(const std::string& j, uint32_t& w, uint32_t& h) {
    const size_t n = j.size();
    auto u = [&](size_t i) { return static_cast<unsigned char>(j[i]); };
    if (n < 4 || u(0) != 0xFF || u(1) != 0xD8) return false;
    if (u(n - 2) != 0xFF || u(n - 1) != 0xD9) return false;                       // incomplete frame: no EOI
    size_t i = 2;
    bool have_dims = false;
    while (i + 4 <= n) {
        if (u(i) != 0xFF) return false;
        while (i < n && u(i) == 0xFF) ++i;                                         // fill bytes
        if (i >= n) return false;
        const unsigned char m = u(i++);
        if (m == 0xD9) break;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;                       // markers without a length
        if (i + 2 > n) return false;
        const size_t len = (static_cast<size_t>(u(i)) << 8) | u(i + 1);
        if (len < 2 || i + len > n) return false;
        if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (len < 8) return false;
            h = (static_cast<uint32_t>(u(i + 3)) << 8) | u(i + 4);
            w = (static_cast<uint32_t>(u(i + 5)) << 8) | u(i + 6);
            have_dims = true;
        }
        if (m == 0xDA) break;                                                       // start of scan: entropy data follows
        i += len;
    }
    return have_dims && w > 0 && h > 0;
}

DisplayFrameSource::DisplayFrameSource(DisplayCfg cfg, std::unique_ptr<FrameClient> client)
    : cfg_(std::move(cfg)), client_(std::move(client)) {
    cfg_.poll_ms = std::max(50, std::min(2000, cfg_.poll_ms));
    cfg_.ring_frames = std::max(2, std::min(64, cfg_.ring_frames));
}
DisplayFrameSource::~DisplayFrameSource() { stop(); }

void DisplayFrameSource::start() {
    if (th_.joinable() || !client_) return;
    stop_ = false;
    th_ = std::thread([this] { loop(); });
}
void DisplayFrameSource::stop() {
    stop_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void DisplayFrameSource::touch_preview() {
    { std::lock_guard<std::mutex> lk(mu_); last_preview_touch_ = mono_ms(); }
    cv_.notify_all();
}
void DisplayFrameSource::hold(const std::string& id) {
    { std::lock_guard<std::mutex> lk(mu_); holds_[id] = mono_ms(); }
    cv_.notify_all();
}
void DisplayFrameSource::release(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    holds_.erase(id);
}

bool DisplayFrameSource::active_locked() {
    if (!holds_.empty()) return true;
    return last_preview_touch_ >= 0 && mono_ms() - last_preview_touch_ < cfg_.linger_ms;
}

void DisplayFrameSource::set_state(const std::string& s, const std::string& detail) {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = s; detail_ = detail;
    if (s != "ok" && s != "idle") last_error_ = detail;
}

void DisplayFrameSource::loop() {
    while (!stop_) {
        bool act;
        { std::lock_guard<std::mutex> lk(mu_); act = active_locked(); }
        if (!act) {
            std::unique_lock<std::mutex> lk(mu_);
            if (state_ != "idle") { state_ = "idle"; detail_ = "no viewer and no Proof of Play session: not polling"; }
            cv_.wait_for(lk, std::chrono::milliseconds(250));
            continue;
        }
        const int64_t t0 = mono_ms();
        poll_cycle();
        int wait = cfg_.poll_ms;
        { std::lock_guard<std::mutex> lk(mu_); if (backoff_ms_ > 0) wait = std::max(wait, backoff_ms_); }
        const int64_t spent = mono_ms() - t0;
        if (spent < wait) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(wait - spent), [&] { return stop_.load(); });
        }
    }
}

void DisplayFrameSource::poll_cycle() {
    if (!client_) return;
    const int64_t now = mono_ms();
    { std::lock_guard<std::mutex> lk(mu_); last_poll_ms_ = now; }
    if (now - last_interest_ms_ >= cfg_.interest_every_ms) { client_->signal_interest(); last_interest_ms_ = now; }

    Health h1;
    const int64_t t0 = mono_ms();
    const bool ok1 = client_->health(h1);
    const int64_t tmid = mono_ms();
    if (!ok1 || !h1.rpc_ok) {
        std::lock_guard<std::mutex> lk(mu_);
        ++rpc_errors_; state_ = "unavailable";
        detail_ = "cannot reach the OS rendered-frame service (" + (h1.error_code.empty() ? std::string("no reply") : h1.error_code) + ")";
        last_error_ = detail_;
        backoff_ms_ = std::min(2000, backoff_ms_ ? backoff_ms_ * 2 : cfg_.poll_ms * 2);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        backoff_ms_ = 0;
        last_health_ = h1;
    }
    // The daemon's "now" must be our monotonic clock; otherwise capture times cannot be trusted.
    if (std::llabs(static_cast<int64_t>(h1.daemon_mono_ms) - (t0 + tmid) / 2) > cfg_.clock_tolerance_ms) {
        std::lock_guard<std::mutex> lk(mu_);
        ++clock_rejects_; state_ = "clock_mismatch";
        detail_ = "OS service clock differs from this process by " + std::to_string(static_cast<int64_t>(h1.daemon_mono_ms) - (t0 + tmid) / 2) +
                  " ms: frame times are not trusted";
        last_error_ = detail_;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!h1.boot_id.empty() && boot_id_ != h1.boot_id) {
            if (!boot_id_.empty()) { ++restarts_; last_freshness_ = 0; ring_.clear(); ring_bytes_ = 0; }
            boot_id_ = h1.boot_id;
        }
    }
    if (!h1.capture_available) {
        const bool stale = h1.error_code == "STALE";
        std::lock_guard<std::mutex> lk(mu_);
        ++stale_polls_;
        state_ = stale ? "stale" : (h1.error_code == "UNAVAILABLE" || h1.error_code.empty() ? "unavailable" : "error");
        detail_ = "OS service reports " + (h1.error_code.empty() ? std::string("no frame") : h1.error_code) +
                  (h1.error_message.empty() ? std::string() : ": " + h1.error_message);
        last_error_ = detail_;
        return;
    }
    if (std::find(cfg_.accept_providers.begin(), cfg_.accept_providers.end(), h1.provider) == cfg_.accept_providers.end()) {
        std::lock_guard<std::mutex> lk(mu_);
        ++rejected_provider_; state_ = "provider_rejected";
        detail_ = "frame provenance " + std::to_string(h1.provider) + " is not an accepted display source";
        last_error_ = detail_;
        return;
    }
    if (h1.freshness_mono_ms == last_freshness_) {                                  // no new frame since the last accepted one
        std::lock_guard<std::mutex> lk(mu_);
        ++same_skipped_; state_ = "ok"; detail_.clear();
        return;
    }

    RawFrame f;
    const bool okc = client_->capture(f);
    Health h2;
    const bool ok2 = client_->health(h2);
    const int64_t t1 = mono_ms();
    if (!okc || !f.ok) {
        std::lock_guard<std::mutex> lk(mu_);
        ++rpc_errors_; state_ = f.error_code == "STALE" ? "stale" : "unavailable";
        detail_ = "capture failed (" + (f.error_code.empty() ? std::string("no frame") : f.error_code) + ")";
        last_error_ = detail_;
        return;
    }
    if (!ok2 || !h2.rpc_ok || h2.freshness_mono_ms != h1.freshness_mono_ms) {       // a newer frame arrived during the call: which one we got is unknown
        std::lock_guard<std::mutex> lk(mu_);
        ++ambiguous_;
        return;
    }
    if (f.jpeg.size() > cfg_.max_jpeg_bytes) { std::lock_guard<std::mutex> lk(mu_); ++oversize_; last_freshness_ = h1.freshness_mono_ms; return; }
    uint32_t w = 0, h = 0;
    if (!jpeg_info(f.jpeg, w, h) ||
        std::find(cfg_.accept_providers.begin(), cfg_.accept_providers.end(), f.provider) == cfg_.accept_providers.end()) {
        std::lock_guard<std::mutex> lk(mu_);
        ++invalid_; last_freshness_ = h1.freshness_mono_ms;
        state_ = "error"; detail_ = "the OS service returned a frame that is not a complete JPEG or has an unexpected provenance";
        last_error_ = detail_;
        return;
    }

    DisplayFrame fr;
    const int64_t m_now = mono_ms(), u_now = utc_ms();
    fr.capture_mono_ms = static_cast<int64_t>(h1.freshness_mono_ms);
    fr.capture_utc_ms = u_now - (m_now - fr.capture_mono_ms);
    fr.acquire_ms = t1 - t0;
    fr.uncertainty_ms = fr.acquire_ms + cfg_.producer_latency_ms;
    fr.jpeg = std::make_shared<const std::string>(std::move(f.jpeg));
    fr.width = w; fr.height = h; fr.provider = f.provider; fr.boot_id = h1.boot_id; fr.health = h1;
    std::lock_guard<std::mutex> lk(mu_);
    fr.seq = ++seq_;
    last_freshness_ = h1.freshness_mono_ms;
    ring_bytes_ += fr.jpeg->size();
    ring_.push_back(std::move(fr));
    while (ring_.size() > static_cast<size_t>(cfg_.ring_frames) || ring_bytes_ > static_cast<size_t>(cfg_.ring_frames) * cfg_.max_jpeg_bytes) {
        ring_bytes_ -= ring_.front().jpeg->size();
        ring_.pop_front();
    }
    ++accepted_; last_accept_mono_ = m_now; state_ = "ok"; detail_.clear();
    accept_times_.push_back(m_now);
    while (!accept_times_.empty() && m_now - accept_times_.front() > 5000) accept_times_.pop_front();
    acquire_samples_.push_back(t1 - t0);
    if (acquire_samples_.size() > 200) acquire_samples_.pop_front();
}

bool DisplayFrameSource::latest(DisplayFrame& out, int64_t& age_ms, std::string& why) {
    std::lock_guard<std::mutex> lk(mu_);
    if (ring_.empty()) { why = "no frame yet (" + state_ + (detail_.empty() ? "" : ": " + detail_) + ")"; return false; }
    out = ring_.back();
    age_ms = mono_ms() - out.capture_mono_ms;
    if (age_ms > cfg_.max_age_ms) { why = "newest frame is " + std::to_string(age_ms) + " ms old"; return false; }
    return true;
}

bool DisplayFrameSource::newer_than(uint64_t seq, DisplayFrame& out, int64_t& age_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    if (ring_.empty() || ring_.back().seq <= seq) return false;
    out = ring_.back();
    age_ms = mono_ms() - out.capture_mono_ms;
    return age_ms <= cfg_.max_age_ms;
}

std::optional<DisplayFrame> DisplayFrameSource::nearest(int64_t target_utc_ms, int64_t before_ms, int64_t after_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    const DisplayFrame* best = nullptr;
    int64_t bd = INT64_MAX;
    for (const auto& f : ring_) {
        const int64_t d = f.capture_utc_ms - target_utc_ms;
        if (d < -before_ms || d > after_ms) continue;
        if (std::llabs(d) < bd) { bd = std::llabs(d); best = &f; }
    }
    if (!best) return std::nullopt;
    return *best;
}

json DisplayFrameSource::status() {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t now = mono_ms();
    json j;
    j["enabled"] = cfg_.enabled;
    j["backend"] = cfg_.backend;
    j["client"] = client_ ? client_->describe() : std::string("none");
    j["state"] = state_;
    j["detail"] = detail_;
    j["active"] = active_locked();
    j["source"] = "Weston VOP2 writeback via the OS Resilience service (approximate composed display output, not a bit-exact HDMI readback)";
    double fps = 0;
    if (accept_times_.size() >= 2) {
        const double span = (accept_times_.back() - accept_times_.front()) / 1000.0;
        if (span > 0) fps = (accept_times_.size() - 1) / span;
    }
    j["fps"] = std::round(fps * 10) / 10.0;
    std::vector<int64_t> a(acquire_samples_.begin(), acquire_samples_.end());
    std::sort(a.begin(), a.end());
    auto pct = [&](double p) { return a.empty() ? json(nullptr) : json(a[std::min(a.size() - 1, static_cast<size_t>(p * a.size()))]); };
    j["acquire_ms"] = {{"p50", pct(0.5)}, {"p95", pct(0.95)}, {"samples", a.size()}};
    j["counters"] = {{"accepted", accepted_}, {"same_frame_skipped", same_skipped_}, {"ambiguous_discarded", ambiguous_}, {"rpc_errors", rpc_errors_},
                     {"invalid_jpeg", invalid_}, {"provider_rejected", rejected_provider_}, {"clock_rejected", clock_rejects_},
                     {"stale_polls", stale_polls_}, {"service_restarts", restarts_}, {"oversize", oversize_}};
    j["ring"] = {{"frames", ring_.size()}, {"bytes", ring_bytes_}, {"capacity", cfg_.ring_frames}};
    if (!ring_.empty()) {
        const auto& f = ring_.back();
        j["last_frame"] = {{"seq", f.seq}, {"capture_utc_ms", f.capture_utc_ms}, {"age_ms", now - f.capture_mono_ms}, {"width", f.width},
                           {"height", f.height}, {"bytes", f.jpeg->size()}, {"uncertainty_ms", f.uncertainty_ms}, {"acquire_ms", f.acquire_ms},
                           {"provider", f.provider}};
    } else j["last_frame"] = nullptr;
    j["health"] = {{"capture_available", last_health_.capture_available}, {"error_code", last_health_.error_code},
                   {"sample_fps", last_health_.sample_fps}, {"avg_luminance", last_health_.avg_luminance},
                   {"frame_diff_score", last_health_.frame_diff}, {"black_screen", last_health_.black}, {"frozen_frame", last_health_.frozen},
                   {"black_duration_ms", last_health_.black_ms}, {"frozen_duration_ms", last_health_.frozen_ms}};
    j["holds"] = holds_.size();
    j["last_error"] = last_error_;
    j["config"] = {{"poll_ms", cfg_.poll_ms}, {"max_age_ms", cfg_.max_age_ms}, {"ring_frames", cfg_.ring_frames},
                   {"producer_latency_ms", cfg_.producer_latency_ms}, {"producer_latency_measured", false}};
    // Display health in one word for the dashboard: available | stale | unavailable
    std::string dh = state_ == "ok" ? "available" : (state_ == "idle" ? (ring_.empty() ? "idle" : "idle") : state_ == "stale" ? "stale" : "unavailable");
    if (state_ == "ok" && !ring_.empty() && now - ring_.back().capture_mono_ms > cfg_.max_age_ms) dh = "stale";
    j["display_health"] = dh;
    return j;
}

}  // namespace argus_display
