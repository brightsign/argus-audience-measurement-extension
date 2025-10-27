#include "output/brightsign_v3_publisher.h"

struct BrightSignV3Publisher::Impl {
  BrightSignV3Config cfg;
  bool started{false};
  Impl(BrightSignV3Config c):cfg(std::move(c)){}
  // TODO: HTTP/IPC call to BrightScript presentation
};

BrightSignV3Publisher::BrightSignV3Publisher(const BrightSignV3Config& cfg) noexcept : p_(new Impl(cfg)) {}
BrightSignV3Publisher::~BrightSignV3Publisher() = default;

bool BrightSignV3Publisher::start() noexcept { p_->started = true; return true; }
void BrightSignV3Publisher::stop() noexcept { p_->started = false; }
bool BrightSignV3Publisher::publish_result(const PipelineResult&) noexcept { return p_->started; }
bool BrightSignV3Publisher::publish_telemetry(const TelemetrySnapshot&) noexcept { return p_->started; }
bool BrightSignV3Publisher::publish_log(const LogRecord&) noexcept { return p_->started; }

