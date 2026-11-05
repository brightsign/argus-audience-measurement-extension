// SPDX-License-Identifier: MIT
// DisplayFrameSource: pulls composed display-output frames from the OS Resilience rendered-frame service (through a
// FrameClient) for the dashboard "Display output" preview and for Proof of Play evidence.
//
// Deliberately free of gRPC, of BrightScript and of libvisualmemory: the OS contract is reached through FrameClient
// (the bridge client in production, a fake in tests). Everything here runs on its own thread inside vm_api_server and
// never touches the Argus capture/inference threads.
//
// What the OS service does and does NOT tell us (see docs/RESILIENCE_DISPLAY_FRAMES_FEASIBILITY.md):
//  * the frame message has no timestamp; the health message has the daemon's receipt time of the newest frame
//    (freshness_timestamp_ms, monotonic ms) and the daemon's current time (monotonic_timestamp_ms);
//  * so a frame is accepted only when Health(before) and Health(after) the Capture call report the same freshness
//    timestamp, and its capture time is that freshness timestamp (an upper bound: the real display moment is earlier by
//    the unmeasured producer latency, recorded as an uncertainty);
//  * the daemon's clock must be the same kernel monotonic clock as ours; this is checked on every poll.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace argus_display {

// ---- what a client returns -------------------------------------------------------------------------------------------
struct Health {
    bool        rpc_ok = false;             // the call itself worked
    bool        capture_available = false;  // daemon has a fresh servable frame
    std::string error_code, error_message;  // daemon / transport reason when not available ("STALE", "UNAVAILABLE", "RPC_14", ...)
    std::string boot_id;
    uint64_t    daemon_mono_ms = 0;         // daemon's "now" at the call (CLOCK_MONOTONIC ms)
    uint64_t    freshness_mono_ms = 0;      // daemon's receipt time of the newest frame (0 = never)
    int         provider = 0;               // SourceProvider enum value (7 = weston vop2 writeback)
    uint32_t    width = 0, height = 0;
    double      sample_fps = 0, avg_luminance = 0, frame_diff = 0;
    bool        black = false, frozen = false;
    uint64_t    black_ms = 0, frozen_ms = 0;
};

struct RawFrame {
    bool        ok = false;
    std::string jpeg;
    uint32_t    width = 0, height = 0;
    std::string mime, error_code;
    int         provider = 0;
};

class FrameClient {
public:
    virtual ~FrameClient() = default;
    virtual bool health(Health& out) = 0;       // false = transport failure (out.error_code says why)
    virtual bool capture(RawFrame& out) = 0;    // false = transport failure or no frame
    virtual void signal_interest() = 0;         // keep the OS producer capturing (best effort)
    virtual std::string describe() const = 0;
};

// ---- configuration -------------------------------------------------------------------------------------------------
struct DisplayCfg {
    bool        enabled = false;                                  // visual_memory.display.enabled
    std::string backend = "bridge";                               // "bridge" (argus_display_bridge) | "none"
    std::string bridge_socket = "/tmp/argus_display_bridge.sock";
    int         poll_ms = 200;                                    // 50..2000; the OS producer runs at 5 FPS
    int         max_age_ms = 1200;                                // a frame older than this is not served as current
    int         ring_frames = 16;                                 // bounded in-memory ring (preview + Proof of Play window)
    size_t      max_jpeg_bytes = 1u << 20;                        // OS slot stride is 1 MiB
    int         linger_ms = 3000;                                 // keep polling this long after the last preview request
    int         interest_every_ms = 2000;                         // producer interest window is 5 s
    int         clock_tolerance_ms = 1500;                        // daemon clock vs ours at the call
    int         producer_latency_ms = 250;                        // budget for capture+encode+notify before the daemon receives a frame. NOT measured yet.
    std::vector<int> accept_providers = {7};                      // SOURCE_PROVIDER_WESTON_VOP2_WRITEBACK
};

struct DisplayFrame {
    uint64_t    seq = 0;                    // local, strictly increasing
    int64_t     capture_mono_ms = 0;        // = daemon freshness timestamp (monotonic)
    int64_t     capture_utc_ms = 0;         // converted with our own realtime-monotonic offset at acquisition
    int64_t     uncertainty_ms = 0;         // rpc span + producer latency budget: capture time may be this much later than the true display moment
    int64_t     acquire_ms = 0;             // Health+Capture+Health round trip
    std::shared_ptr<const std::string> jpeg;
    uint32_t    width = 0, height = 0;
    int         provider = 0;
    std::string boot_id;
    Health      health;
};

int64_t mono_ms();                          // CLOCK_MONOTONIC in ms (std::chrono::steady_clock on Linux)
int64_t utc_ms();                           // wall clock in ms
bool    jpeg_info(const std::string& jpeg, uint32_t& w, uint32_t& h);   // SOI ... SOF ... EOI check, false = not a complete JPEG

class DisplayFrameSource {
public:
    DisplayFrameSource(DisplayCfg cfg, std::unique_ptr<FrameClient> client);
    ~DisplayFrameSource();
    void start();
    void stop();

    // Interest. Nothing is polled (no RPC, no producer capture) unless a preview was touched within linger_ms or a hold is active.
    void touch_preview();
    void hold(const std::string& id);       // Proof of Play session window
    void release(const std::string& id);

    // The newest frame; false when none, or older than max_age_ms (then `why` says so). age_ms is relative to now.
    bool latest(DisplayFrame& out, int64_t& age_ms, std::string& why);
    bool newer_than(uint64_t seq, DisplayFrame& out, int64_t& age_ms);
    // The frame closest to `target_utc_ms` inside [target - before_ms, target + after_ms], if any.
    std::optional<DisplayFrame> nearest(int64_t target_utc_ms, int64_t before_ms, int64_t after_ms);

    nlohmann::json status();
    const DisplayCfg& cfg() const { return cfg_; }

    // test hook: run exactly one poll cycle synchronously (no thread)
    void poll_once_for_test() { poll_cycle(); }

private:
    void loop();
    void poll_cycle();
    bool active_locked();
    void set_state(const std::string& s, const std::string& detail);

    DisplayCfg cfg_;
    std::unique_ptr<FrameClient> client_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<DisplayFrame> ring_;
    size_t ring_bytes_ = 0;
    uint64_t seq_ = 0;
    int64_t last_preview_touch_ = -1;
    std::map<std::string, int64_t> holds_;
    int64_t last_interest_ms_ = -1000000;
    uint64_t last_freshness_ = 0;
    std::string boot_id_;
    // state / stats
    std::string state_ = "idle", detail_;
    Health last_health_;
    int64_t last_poll_ms_ = 0, last_accept_mono_ = 0;
    uint64_t accepted_ = 0, same_skipped_ = 0, ambiguous_ = 0, rpc_errors_ = 0, invalid_ = 0, rejected_provider_ = 0,
             clock_rejects_ = 0, stale_polls_ = 0, restarts_ = 0, oversize_ = 0;
    std::deque<int64_t> accept_times_;
    std::deque<int64_t> acquire_samples_;
    std::string last_error_;
    int backoff_ms_ = 0;
};

}  // namespace argus_display
