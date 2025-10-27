#ifndef TELEMETRY_H
#define TELEMETRY_H

#include "metrics/metrics_types.h"

// Abstract telemetry sink — plug implementations in .cpp (UDP/JSON, MQTT, etc.)
class ITelemetrySink {
public:
  virtual ~ITelemetrySink() = default;
  virtual bool start() noexcept = 0;
  virtual void stop() noexcept = 0;

  // Send a full snapshot (e.g., once per second)
  virtual bool publish(const TelemetrySnapshot& snap) noexcept = 0;

  // Optional: publish detailed histograms when debug enabled
  virtual bool publish_histogram(Stage stage,
                                 const HistogramConfig& cfg,
                                 const uint32_t* bins, uint16_t n) noexcept {
    (void)stage; (void)cfg; (void)bins; (void)n; return true;
  }
};

#endif // TELEMETRY_H

