#ifndef PUBLISHER_H
#define PUBLISHER_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include "output/output_types.h"
#include "pipeline/pipeline_types.h"
#include "metrics/metrics_types.h"

// forward decls to avoid heavy includes
struct PipelineResult;
struct TelemetrySnapshot;

// Base interface for all publishers (UDP, BrightSign, files, etc.)
class IPublisher {
public:
  virtual ~IPublisher() = default;

  // Lifecycle
  virtual bool start() noexcept = 0;
  virtual void stop() noexcept = 0;

  // Non-blocking publish requests. Return false if dropped/refused.
  virtual bool publish_result(const PipelineResult& r) noexcept { (void)r; return true; }
  virtual bool publish_telemetry(const TelemetrySnapshot& t) noexcept { (void)t; return true; }
  virtual bool publish_log(const LogRecord& rec) noexcept { (void)rec; return true; }

  // Generic blob (e.g., small JSON string, binary stats). Ownership stays with caller.
  virtual bool publish_blob(OutputFormat fmt, const void* data, size_t len) noexcept {
    (void)fmt; (void)data; (void)len; return true;
  }
};

// Factory-friendly alias
using PublisherPtr = std::unique_ptr<IPublisher>;

#endif // PUBLISHER_H

