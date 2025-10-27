#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <memory>
#include "output/output_types.h"

// Optional diagnostics interface for development builds.
// Could stream RTSP/PCAP stats, store short rolling traces, etc.
class IDiagnosticsSink {
public:
  virtual ~IDiagnosticsSink() = default;
  virtual bool start() noexcept = 0;
  virtual void stop() noexcept = 0;

  virtual bool report_rtsp(const RtspStats& s) noexcept = 0;
  virtual bool report_blob(const char* name, const void* data, size_t len) noexcept = 0;
};

// Factory (implement in .cpp)
std::unique_ptr<IDiagnosticsSink> make_dev_diagnostics_udp(const char* host, uint16_t port) noexcept;

#endif // DIAGNOSTICS_H

