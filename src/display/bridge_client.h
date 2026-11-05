// SPDX-License-Identifier: MIT
// FrameClient that talks to argus_display_bridge over a local Unix-domain socket. No gRPC dependency.
#pragma once

#include <memory>
#include <string>

#include "display/display_frame_source.h"

namespace argus_display {

class BridgeClient : public FrameClient {
public:
    explicit BridgeClient(std::string socket_path, int timeout_ms = 1500, size_t max_jpeg = 1u << 20)
        : path_(std::move(socket_path)), timeout_ms_(timeout_ms), max_jpeg_(max_jpeg) {}
    ~BridgeClient() override;
    bool health(Health& out) override;
    bool capture(RawFrame& out) override;
    void signal_interest() override;
    std::string describe() const override { return "bridge " + path_; }

private:
    bool ensure();
    bool call(char op, std::string& json_text, std::string& jpeg);
    void drop();
    std::string path_;
    int timeout_ms_;
    size_t max_jpeg_;
    int fd_ = -1;
};

}  // namespace argus_display
