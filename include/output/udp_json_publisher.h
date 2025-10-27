#ifndef UDP_JSON_PUBLISHER_H
#define UDP_JSON_PUBLISHER_H

#include <memory>
#include <string>
#include "output/publisher_v2.h"

// UDP JSON publisher (real-time, low-latency)
// Implementation hides sockets/serialization in Impl.
struct UdpEndpoint {
  std::string host{"127.0.0.1"};
  uint16_t    port{5555};
};

class UdpJsonPublisher final : public IPublisher {
public:
  explicit UdpJsonPublisher(const UdpEndpoint& ep) noexcept;
  ~UdpJsonPublisher() override;

  bool start() noexcept override;
  void stop() noexcept override;

  bool publish_result(const PipelineResult& r) noexcept override;
  bool publish_telemetry(const TelemetrySnapshot& t) noexcept override;
  bool publish_log(const LogRecord& rec) noexcept override;
  bool publish_blob(OutputFormat fmt, const void* data, size_t len) noexcept override;

private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

#endif // UDP_JSON_PUBLISHER_H

