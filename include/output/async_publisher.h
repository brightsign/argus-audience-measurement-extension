#ifndef ASYNC_PUBLISHER_H
#define ASYNC_PUBLISHER_H

#include <atomic>
#include <thread>
#include <vector>
#include <variant>
#include <memory>
#include "output/publisher_v2.h"


// Asynchronous wrapper with a bounded queue and drop-old policy.
// Ensures the pipeline never blocks on slow sinks.
class AsyncPublisher final : public IPublisher {
public:
  struct Config {
    size_t queue_capacity{64};   // number of messages buffered
  };

  explicit AsyncPublisher(PublisherPtr inner) noexcept;                    // uses default Config{}
  AsyncPublisher(PublisherPtr inner, const Config& cfg) noexcept;          // explicit cfg

  ~AsyncPublisher() override { stop(); }

  bool start() noexcept override;
  void stop() noexcept override;

  bool publish_result(const PipelineResult& r) noexcept override;
  bool publish_telemetry(const TelemetrySnapshot& t) noexcept override;
  bool publish_log(const LogRecord& rec) noexcept override;
  bool publish_blob(OutputFormat fmt, const void* data, size_t len) noexcept override;

private:
  struct MsgBlob { OutputFormat fmt; const void* data; size_t len; };
  using Msg = std::variant<PipelineResult, TelemetrySnapshot, LogRecord, MsgBlob>;

  void run() noexcept;
  bool enqueue(Msg&& m) noexcept;

  PublisherPtr inner_;
  Config cfg_{};

  std::thread th_;
  std::atomic<bool> stop_{false};

  // Simple ring with drop-old
  std::vector<Msg> q_;
  std::atomic<size_t> r_{0}, w_{0};
};

#endif // ASYNC_PUBLISHER_H

