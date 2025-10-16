#ifndef BRIGHTSIGN_V3_PUBLISHER_H
#define BRIGHTSIGN_V3_PUBLISHER_H

#include <memory>
#include <string>
#include "publisher.h"

// BrightSign V3 integration.
// Typical patterns: BrightScript variables/events via local endpoints,
// or writing to device variables/watchers. Details hidden in Impl.
struct BrightSignV3Config {
  // Example knobs (adapt to your environment):
  // local HTTP endpoint or IPC path the BrightScript presentation listens to
  std::string local_endpoint{"http://127.0.0.1:8999/notify"};
  // variable names/topics
  std::string var_result{"ml_result"};
  std::string var_telemetry{"ml_telemetry"};
  bool compact_json{true};
};

class BrightSignV3Publisher final : public IPublisher {
public:
  explicit BrightSignV3Publisher(const BrightSignV3Config& cfg) noexcept;
  ~BrightSignV3Publisher() override;

  bool start() noexcept override;
  void stop() noexcept override;

  bool publish_result(const PipelineResult& r) noexcept override;
  bool publish_telemetry(const TelemetrySnapshot& t) noexcept override;
  bool publish_log(const LogRecord& rec) noexcept override;

private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

#endif // BRIGHTSIGN_V3_PUBLISHER_H

