#pragma once
#include "output/publisher_v2.h"
#include <mosquitto.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>

struct MqttPublisherCfg {
  std::string host{"127.0.0.1"};
  int         port{1883};
  std::string client_id{"xt5-gaze"};
  std::string topic{"bs/argus/analytics"};
  int         qos{1};
  bool        retain{false};
  int         period_ms{1000};
  std::string username{};
  std::string password{};
  bool        clean_session{true};
};

class MqttPublisher final : public IPublisher {
public:
  using Cfg = MqttPublisherCfg;

  explicit MqttPublisher(const Cfg& cfg) noexcept;
  ~MqttPublisher() override;

  bool start() noexcept override;
  void stop() noexcept override;

  bool publish_result(const PipelineResult& r) noexcept override;
  bool publish_telemetry(const TelemetrySnapshot& t) noexcept override;

private:
  void tick_publish() noexcept;
  std::string make_payload_locked() const;

  Cfg cfg_;
  mosquitto* mq_{nullptr};
  std::atomic<bool> run_{false};
  std::thread tick_;

  mutable std::mutex m_;
  int      people_{0};
  int      gaze_{0};
  int      frames_accum_{0};
  uint64_t last_ts_ns_{0};
};
