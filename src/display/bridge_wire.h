// SPDX-License-Identifier: MIT
// Wire format between vm_api_server (BridgeClient) and argus_display_bridge. Local Unix-domain stream socket only.
//
//   request : 1 byte op  'H' = GetRenderedFrameHealth, 'C' = CaptureRenderedFrame, 'I' = touch the producer interest file
//   response: u32 (big endian) JSON length, JSON, u32 JPEG length, JPEG bytes (length 0 when there is no frame)
//
// JSON always carries "rpc_ok" (the gRPC call itself worked) and "error_code". This file has no gRPC dependency so both
// sides and the tests share one definition.
#pragma once

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include "display/display_frame_source.h"

namespace argus_display::wire {

inline bool write_all(int fd, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        const ssize_t w = ::send(fd, c, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        c += w; n -= static_cast<size_t>(w);
    }
    return true;
}
inline bool read_all(int fd, void* p, size_t n, int timeout_ms) {
    char* c = static_cast<char*>(p);
    while (n) {
        pollfd pf{fd, POLLIN, 0};
        const int r = ::poll(&pf, 1, timeout_ms);
        if (r <= 0) return false;
        const ssize_t g = ::recv(fd, c, n, 0);
        if (g < 0) { if (errno == EINTR) continue; return false; }
        if (g == 0) return false;
        c += g; n -= static_cast<size_t>(g);
    }
    return true;
}
inline void put_u32(std::string& s, uint32_t v) {
    const char b[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
    s.append(b, 4);
}
inline uint32_t get_u32(const unsigned char* b) { return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3]; }

inline std::string frame_response(const std::string& json_text, const std::string& jpeg) {
    std::string out;
    put_u32(out, static_cast<uint32_t>(json_text.size()));
    out += json_text;
    put_u32(out, static_cast<uint32_t>(jpeg.size()));
    out += jpeg;
    return out;
}

// Reads one response. Limits keep a broken peer from making us allocate without bound.
inline bool read_response(int fd, std::string& json_text, std::string& jpeg, int timeout_ms, size_t max_jpeg) {
    unsigned char h[4];
    if (!read_all(fd, h, 4, timeout_ms)) return false;
    const uint32_t jl = get_u32(h);
    if (jl > (1u << 16)) return false;
    json_text.assign(jl, '\0');
    if (jl && !read_all(fd, &json_text[0], jl, timeout_ms)) return false;
    if (!read_all(fd, h, 4, timeout_ms)) return false;
    const uint32_t pl = get_u32(h);
    if (pl > max_jpeg) return false;
    jpeg.assign(pl, '\0');
    if (pl && !read_all(fd, &jpeg[0], pl, timeout_ms)) return false;
    return true;
}

inline nlohmann::json health_json(const Health& h) {
    return {{"rpc_ok", h.rpc_ok}, {"capture_available", h.capture_available}, {"error_code", h.error_code}, {"error_message", h.error_message},
            {"boot_id", h.boot_id}, {"daemon_mono_ms", h.daemon_mono_ms}, {"freshness_mono_ms", h.freshness_mono_ms}, {"provider", h.provider},
            {"width", h.width}, {"height", h.height}, {"sample_fps", h.sample_fps}, {"avg_luminance", h.avg_luminance}, {"frame_diff", h.frame_diff},
            {"black", h.black}, {"frozen", h.frozen}, {"black_ms", h.black_ms}, {"frozen_ms", h.frozen_ms}};
}
inline Health health_from_json(const nlohmann::json& j) {
    Health h;
    auto s = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    auto u = [&](const char* k) -> uint64_t { return j.contains(k) && j[k].is_number() ? j[k].get<uint64_t>() : 0; };
    auto d = [&](const char* k) { return j.contains(k) && j[k].is_number() ? j[k].get<double>() : 0.0; };
    auto b = [&](const char* k) { return j.contains(k) && j[k].is_boolean() && j[k].get<bool>(); };
    h.rpc_ok = b("rpc_ok"); h.capture_available = b("capture_available"); h.error_code = s("error_code"); h.error_message = s("error_message");
    h.boot_id = s("boot_id"); h.daemon_mono_ms = u("daemon_mono_ms"); h.freshness_mono_ms = u("freshness_mono_ms"); h.provider = static_cast<int>(u("provider"));
    h.width = static_cast<uint32_t>(u("width")); h.height = static_cast<uint32_t>(u("height")); h.sample_fps = d("sample_fps");
    h.avg_luminance = d("avg_luminance"); h.frame_diff = d("frame_diff"); h.black = b("black"); h.frozen = b("frozen");
    h.black_ms = u("black_ms"); h.frozen_ms = u("frozen_ms");
    return h;
}

}  // namespace argus_display::wire
