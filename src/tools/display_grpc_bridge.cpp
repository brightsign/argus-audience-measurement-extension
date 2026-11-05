// SPDX-License-Identifier: MIT
// argus_display_bridge: the ONLY component of this feature that depends on gRPC.
//
// Forwards three operations from vm_api_server (local Unix socket, see display/bridge_wire.h) to the OS Resilience
// RenderedOutputService (gRPC over unix:///run/brightsign/resilience/runtime_api.sock):
//   'H' GetRenderedFrameHealth    'C' CaptureRenderedFrame    'I' touch the producer interest file
// It is read-only with respect to the player: the only filesystem write is the interest-file touch the OS design asks
// consumers to do (create if missing, bump mtime). No TCP listener, no state, no decoding. Pattern follows the Resilience
// extension's RenderedOutputGrpcClient (channel, deadline, interest touch), but keeps the health metadata that its LiveFrame drops.
//
//   argus_display_bridge [--socket /tmp/argus_display_bridge.sock] [--target unix:///run/brightsign/resilience/runtime_api.sock]
//                        [--interest-file /run/brightsign/resilience/rendered_frame.interest] [--deadline-ms 700]
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "brightsign/resilience/v1/rendered_output.grpc.pb.h"
#include "display/bridge_wire.h"

namespace v1 = brightsign::resilience::v1;
using argus_display::Health;
using argus_display::RawFrame;

static std::atomic<bool> g_stop{false};
static void on_sig(int) { g_stop = true; }

struct Upstream {
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<v1::RenderedOutputService::Stub> stub;
    std::string interest_file;
    int deadline_ms;

    Upstream(const std::string& target, std::string interest, int deadline)
        : channel(grpc::CreateChannel(target, grpc::InsecureChannelCredentials())),
          stub(v1::RenderedOutputService::NewStub(channel)), interest_file(std::move(interest)), deadline_ms(deadline) {}

    std::chrono::system_clock::time_point dl() const { return std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_ms); }

    Health health() {
        Health h;
        grpc::ClientContext ctx; ctx.set_deadline(dl());
        v1::GetRenderedFrameHealthRequest req; v1::RenderedFrameHealth r;
        const grpc::Status s = stub->GetRenderedFrameHealth(&ctx, req, &r);
        if (!s.ok()) { h.error_code = (s.error_code() == grpc::StatusCode::UNIMPLEMENTED) ? "UNIMPLEMENTED" : "RPC_" + std::to_string(static_cast<int>(s.error_code())); return h; }
        h.rpc_ok = true; h.capture_available = r.capture_available(); h.error_code = r.error_code(); h.error_message = r.error_message();
        h.boot_id = r.boot_id(); h.daemon_mono_ms = r.monotonic_timestamp_ms(); h.freshness_mono_ms = r.freshness_timestamp_ms();
        h.provider = static_cast<int>(r.provider()); h.width = r.width(); h.height = r.height(); h.sample_fps = r.sample_fps();
        h.avg_luminance = r.avg_luminance(); h.frame_diff = r.frame_diff_score(); h.black = r.black_screen(); h.frozen = r.frozen_frame();
        h.black_ms = r.black_duration_ms(); h.frozen_ms = r.frozen_duration_ms();
        return h;
    }

    // returns the JSON header; `jpeg` is filled when a frame is available
    nlohmann::json capture(std::string& jpeg) {
        grpc::ClientContext ctx; ctx.set_deadline(dl());
        v1::CaptureRenderedFrameRequest req; v1::RenderedFrame r;
        const grpc::Status s = stub->CaptureRenderedFrame(&ctx, req, &r);
        if (!s.ok()) return {{"rpc_ok", false}, {"available", false}, {"error_code", "RPC_" + std::to_string(static_cast<int>(s.error_code()))}};
        nlohmann::json j = {{"rpc_ok", true}, {"available", r.available()}, {"error_code", r.error_code()}, {"error_message", r.error_message()},
                            {"width", r.width()}, {"height", r.height()}, {"mime", r.mime_type()}, {"provider", static_cast<int>(r.provider())}};
        if (r.available()) jpeg = r.data();
        return j;
    }

    void touch_interest() {
        if (interest_file.empty()) return;
        const int fd = ::open(interest_file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0664);
        if (fd < 0) return;
        ::futimens(fd, nullptr);
        ::close(fd);
    }
};

static void serve(int fd, Upstream& up) {
    for (;;) {
        char op;
        if (!argus_display::wire::read_all(fd, &op, 1, 30000)) break;
        std::string jpeg, out;
        if (op == 'H') {
            out = argus_display::wire::frame_response(argus_display::wire::health_json(up.health()).dump(), "");
        } else if (op == 'C') {
            const auto j = up.capture(jpeg);
            out = argus_display::wire::frame_response(j.dump(), jpeg);
        } else if (op == 'I') {
            up.touch_interest();
            out = argus_display::wire::frame_response("{\"rpc_ok\":true}", "");
        } else break;
        if (!argus_display::wire::write_all(fd, out.data(), out.size())) break;
    }
    ::close(fd);
}

int main(int argc, char** argv) {
    std::string socket_path = "/tmp/argus_display_bridge.sock";
    std::string target = "unix:///run/brightsign/resilience/runtime_api.sock";
    std::string interest = "/run/brightsign/resilience/rendered_frame.interest";
    int deadline = 700;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--socket") socket_path = next();
        else if (a == "--target") target = next();
        else if (a == "--interest-file") interest = next();
        else if (a == "--deadline-ms") deadline = std::atoi(next().c_str());
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    std::signal(SIGINT, on_sig); std::signal(SIGTERM, on_sig); std::signal(SIGPIPE, SIG_IGN);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) { std::fprintf(stderr, "socket path too long\n"); return 2; }
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
    ::unlink(socket_path.c_str());
    const int ls = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ls < 0 || ::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(ls, 4) != 0) {
        std::perror("bind/listen"); return 1;
    }
    ::chmod(socket_path.c_str(), 0600);                                     // vm_api_server runs as the same (root) user
    Upstream up(target, interest, deadline);
    std::printf("argus_display_bridge: %s -> %s (deadline %d ms)\n", socket_path.c_str(), target.c_str(), deadline);
    std::fflush(stdout);
    std::atomic<int> conns{0};
    while (!g_stop) {
        pollfd pf{ls, POLLIN, 0};
        if (::poll(&pf, 1, 250) <= 0) continue;
        const int c = ::accept4(ls, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) continue;
        if (conns >= 4) { ::close(c); continue; }                            // bounded
        ++conns;
        std::thread([c, &up, &conns] { serve(c, up); --conns; }).detach();
    }
    ::close(ls);
    ::unlink(socket_path.c_str());
    return 0;
}
