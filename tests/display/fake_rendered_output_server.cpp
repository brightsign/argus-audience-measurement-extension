// SPDX-License-Identifier: MIT
// TEST DOUBLE of the OS bs-resilience-api-daemon + bs-rendered-frame-producer. Implements the real RenderedOutputService
// contract over a Unix-domain socket with the same observable semantics as the OS code (rendered_frame_store.cpp):
//   * frames are produced at 5 FPS ONLY while the interest file's mtime is younger than 5 s (interest gating);
//   * health.freshness_timestamp_ms = receipt time of the newest frame, monotonic_timestamp_ms = the daemon's "now",
//     sequence_number = a per-call counter (NOT a frame sequence);
//   * a frame older than 3 s is STALE and is never served; no frame yet = UNAVAILABLE;
//   * the frame message carries no timestamp.
// Content comes from JPEG files so tests can show "advertisement A / B / black". A control file (JSON, re-read every cycle) injects
// faults. Synthetic images only; this is not hardware evidence.
//
//   fake_rendered_output_server --socket PATH --frames-dir DIR --interest-file PATH --control-file PATH --stats-file PATH
//   control: {"frame":"A","pause":false,"black":false,"frozen":false,"unavailable":false,"clock_skew_ms":0,"publish_delay_ms":0,"provider":7}
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include "brightsign/resilience/v1/rendered_output.grpc.pb.h"

namespace v1 = brightsign::resilience::v1;
namespace fs = std::filesystem;
using json = nlohmann::json;

static std::atomic<bool> g_stop{false};
static void on_sig(int) { g_stop = true; }
static uint64_t mono() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o; o << f.rdbuf();
    return o.str();
}

struct Store {
    std::mutex mu;
    std::string jpeg;
    uint32_t w = 0, h = 0;
    uint64_t last_update = 0;
    bool have = false, black = false, frozen = false;
    int provider = 7;
    uint64_t seq_counter = 0, frames = 0, captures = 0, healths = 0, touches_seen = 0;
    std::string boot_id;
    int64_t skew_ms = 0;
    bool unavailable = false;
};
static Store S;
static std::string g_interest, g_control, g_stats;

static bool dims(const std::string& j, uint32_t& w, uint32_t& h) {
    size_t i = 2;
    auto u = [&](size_t k) { return static_cast<unsigned char>(j[k]); };
    while (i + 9 < j.size()) {
        if (u(i) != 0xFF) { ++i; continue; }
        const unsigned char m = u(i + 1);
        const size_t len = (size_t(u(i + 2)) << 8) | u(i + 3);
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { h = (u(i + 5) << 8) | u(i + 6); w = (u(i + 7) << 8) | u(i + 8); return true; }
        i += 2 + len;
    }
    return false;
}

class Service final : public v1::RenderedOutputService::Service {
    static constexpr uint64_t kStale = 3000;
    grpc::Status GetRenderedFrameHealth(grpc::ServerContext*, const v1::GetRenderedFrameHealthRequest*, v1::RenderedFrameHealth* out) override {
        std::lock_guard<std::mutex> lk(S.mu);
        ++S.healths;
        const uint64_t now = mono() + static_cast<uint64_t>(S.skew_ms);
        out->set_boot_id(S.boot_id);
        out->set_sequence_number(++S.seq_counter);
        out->set_monotonic_timestamp_ms(now);
        if (!S.have) {
            out->set_freshness_timestamp_ms(0); out->set_capture_available(false);
            out->set_provider(v1::SOURCE_PROVIDER_UNAVAILABLE); out->set_error_code("UNAVAILABLE"); out->set_error_message("no producer has reported");
            return grpc::Status::OK;
        }
        out->set_freshness_timestamp_ms(S.last_update);
        out->set_provider(static_cast<v1::SourceProvider>(S.provider));
        out->set_width(S.w); out->set_height(S.h); out->set_sample_fps(5.0);
        out->set_black_screen(S.black); out->set_frozen_frame(S.frozen);
        out->set_avg_luminance(S.black ? 0.0 : 90.0);
        const uint64_t age = now >= S.last_update ? now - S.last_update : 0;
        if (S.unavailable) { out->set_capture_available(false); out->set_error_code("WRITEBACK_UNAVAILABLE"); out->set_error_message("simulated"); }
        else if (age > kStale) { out->set_capture_available(false); out->set_black_screen(false); out->set_frozen_frame(false); out->set_error_code("STALE"); out->set_error_message("producer stopped feeding frames"); }
        else out->set_capture_available(true);
        return grpc::Status::OK;
    }
    grpc::Status CaptureRenderedFrame(grpc::ServerContext*, const v1::CaptureRenderedFrameRequest*, v1::RenderedFrame* out) override {
        std::lock_guard<std::mutex> lk(S.mu);
        ++S.captures;
        const uint64_t now = mono() + static_cast<uint64_t>(S.skew_ms);
        if (!S.have) { out->set_available(false); out->set_provider(v1::SOURCE_PROVIDER_UNAVAILABLE); out->set_error_code("UNAVAILABLE"); return grpc::Status::OK; }
        const uint64_t age = now >= S.last_update ? now - S.last_update : 0;
        out->set_provider(static_cast<v1::SourceProvider>(S.provider));
        if (S.unavailable) { out->set_available(false); out->set_error_code("WRITEBACK_UNAVAILABLE"); return grpc::Status::OK; }
        if (age > kStale) { out->set_available(false); out->set_error_code("STALE"); return grpc::Status::OK; }
        out->set_available(true); out->set_width(S.w); out->set_height(S.h); out->set_mime_type("image/jpeg"); out->set_data(S.jpeg);
        return grpc::Status::OK;
    }
    grpc::Status WatchRenderedFrameHealth(grpc::ServerContext*, const v1::WatchRenderedFrameHealthRequest*,
                                          grpc::ServerWriter<v1::RenderedFrameHealthEvent>*) override {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "poll Get*");
    }
};

static void write_stats() {
    std::lock_guard<std::mutex> lk(S.mu);
    json j = {{"frames_produced", S.frames}, {"capture_calls", S.captures}, {"health_calls", S.healths}, {"boot_id", S.boot_id}};
    std::ofstream(g_stats + ".tmp") << j.dump();
    std::error_code ec; fs::rename(g_stats + ".tmp", g_stats, ec);
}

static void producer(const std::string& dir) {
    uint64_t next = mono();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (mono() < next) continue;
        next = mono() + 200;                                                          // 5 FPS
        // interest gating: only capture while the sentinel is younger than 5 s
        struct stat st{};
        if (::stat(g_interest.c_str(), &st) != 0) { write_stats(); continue; }
        const int64_t age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - std::chrono::system_clock::from_time_t(st.st_mtime)).count() - st.st_mtim.tv_nsec / 1000000 * 0;
        // st_mtime has 1 s resolution in this check; nanoseconds make it exact
        const double mt = st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
        const double nowt = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        (void)age_ms;
        if (nowt - mt > 5.0) { write_stats(); continue; }
        json c;
        try { c = json::parse(read_file(g_control)); } catch (...) { c = json::object(); }
        {
            std::lock_guard<std::mutex> lk(S.mu);
            S.skew_ms = c.value("clock_skew_ms", 0); S.unavailable = c.value("unavailable", false); S.provider = c.value("provider", 7);
        }
        if (c.value("pause", false)) { write_stats(); continue; }                       // producer stalled: daemon goes STALE after 3 s
        const std::string name = c.value("frame", std::string("A"));
        const std::string jpeg = read_file(dir + "/" + name + ".jpg");
        if (jpeg.size() < 4) { write_stats(); continue; }
        const int delay = c.value("publish_delay_ms", 0);
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));   // capture happened earlier than the receipt stamp
        uint32_t w = 0, h = 0; dims(jpeg, w, h);
        {
            std::lock_guard<std::mutex> lk(S.mu);
            S.jpeg = jpeg; S.w = w; S.h = h; S.have = true; S.last_update = mono() + static_cast<uint64_t>(S.skew_ms);
            S.black = c.value("black", false); S.frozen = c.value("frozen", false); ++S.frames;
        }
        write_stats();
    }
}

int main(int argc, char** argv) {
    std::string sock, dir;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i], v = argv[i + 1];
        if (a == "--socket") sock = v; else if (a == "--frames-dir") dir = v; else if (a == "--interest-file") g_interest = v;
        else if (a == "--control-file") g_control = v; else if (a == "--stats-file") g_stats = v;
    }
    if (sock.empty() || dir.empty() || g_interest.empty() || g_control.empty() || g_stats.empty()) { std::cerr << "missing arguments\n"; return 2; }
    std::signal(SIGINT, on_sig); std::signal(SIGTERM, on_sig);
    std::random_device rd; char b[40]; std::snprintf(b, sizeof b, "boot-%08x%08x", rd(), rd()); S.boot_id = b;
    Service svc;
    grpc::ServerBuilder builder;
    ::unlink(sock.c_str());
    builder.AddListeningPort("unix://" + sock, grpc::InsecureServerCredentials());
    builder.RegisterService(&svc);
    auto server = builder.BuildAndStart();
    if (!server) { std::cerr << "cannot start the fake service\n"; return 1; }
    std::cout << "fake rendered-output service listening on " << sock << std::endl;
    std::thread p(producer, dir);
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server->Shutdown();
    p.join();
    return 0;
}
