// SPDX-License-Identifier: MIT
#include "display/bridge_client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include "display/bridge_wire.h"

namespace argus_display {

BridgeClient::~BridgeClient() { drop(); }

void BridgeClient::drop() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool BridgeClient::ensure() {
    if (fd_ >= 0) return true;
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return false;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(a.sun_path)) { drop(); return false; }
    std::memcpy(a.sun_path, path_.c_str(), path_.size() + 1);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { drop(); return false; }
    return true;
}

bool BridgeClient::call(char op, std::string& json_text, std::string& jpeg) {
    if (!ensure()) return false;
    if (!wire::write_all(fd_, &op, 1) || !wire::read_response(fd_, json_text, jpeg, timeout_ms_, max_jpeg_)) { drop(); return false; }
    return true;
}

bool BridgeClient::health(Health& out) {
    std::string j, jpeg;
    if (!call('H', j, jpeg)) { out = Health{}; out.error_code = "BRIDGE_UNREACHABLE"; return false; }
    try { out = wire::health_from_json(nlohmann::json::parse(j)); }
    catch (...) { out = Health{}; out.error_code = "BRIDGE_BAD_REPLY"; drop(); return false; }
    return out.rpc_ok;
}

bool BridgeClient::capture(RawFrame& out) {
    out = RawFrame{};
    std::string j, jpeg;
    if (!call('C', j, jpeg)) { out.error_code = "BRIDGE_UNREACHABLE"; return false; }
    try {
        const auto v = nlohmann::json::parse(j);
        out.error_code = v.value("error_code", std::string());
        out.width = v.value("width", 0u); out.height = v.value("height", 0u);
        out.mime = v.value("mime", std::string()); out.provider = v.value("provider", 0);
        out.ok = v.value("rpc_ok", false) && v.value("available", false) && !jpeg.empty();
    } catch (...) { out.error_code = "BRIDGE_BAD_REPLY"; drop(); return false; }
    if (out.ok) out.jpeg = std::move(jpeg);
    return out.ok;
}

void BridgeClient::signal_interest() {
    std::string j, jpeg;
    call('I', j, jpeg);                       // best effort
}

}  // namespace argus_display
