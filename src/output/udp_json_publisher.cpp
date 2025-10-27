#include "output/udp_json_publisher.h"
#include <cstring>

struct UdpJsonPublisher::Impl {
  UdpEndpoint ep;
  bool started{false};
  Impl(UdpEndpoint e):ep(std::move(e)){}
  // TODO: socket fd + simple JSON encode
};

UdpJsonPublisher::UdpJsonPublisher(const UdpEndpoint& ep) noexcept : p_(new Impl(ep)) {}
UdpJsonPublisher::~UdpJsonPublisher() = default;

bool UdpJsonPublisher::start() noexcept { p_->started = true; return true; }
void UdpJsonPublisher::stop() noexcept  { p_->started = false; }

bool UdpJsonPublisher::publish_result(const PipelineResult&) noexcept { return p_->started; }
bool UdpJsonPublisher::publish_telemetry(const TelemetrySnapshot&) noexcept { return p_->started; }
bool UdpJsonPublisher::publish_log(const LogRecord&) noexcept { return p_->started; }
bool UdpJsonPublisher::publish_blob(OutputFormat, const void*, size_t) noexcept { return p_->started; }

