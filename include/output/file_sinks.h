#ifndef FILE_SINKS_H
#define FILE_SINKS_H

#include <string>
#include <memory>
#include "output/output_types.h"
#include "resources/resource_types.h"
#include "pipeline/pipeline_types.h"

// Writes annotated frames and performance logs to disk with simple rotation.
// Implementation hides filesystem and encoding details (PNG/JPEG).
struct FileSinkConfig {
  std::string dir{"/storage/sd/debug"};
  bool        annotate_png{true};
  bool        dump_raw_frames{false};
  bool        write_perf_log{true};
  size_t      max_bytes_per_file{2 * 1024 * 1024};
  uint32_t    max_files{10};
};

class FileDebugSink {
public:
  explicit FileDebugSink(const FileSinkConfig& cfg) noexcept;
  ~FileDebugSink();

  bool start() noexcept;
  void stop() noexcept;

  // Writes one annotated frame (if enabled). Non-blocking expectation: return false if backpressured.
  bool write_annotated(const ImageBuffer& rgb_or_nv12,
                       const PipelineResult& result,
                       const AnnotationSpec& style) noexcept;

  // Writes a performance log line (CSV/TSV/JSON depending on impl).
  bool write_perf(const LogRecord& perf_line) noexcept;

  // Optionally dump raw bytes (development diagnostics)
  bool dump_frame_raw(const void* data, size_t len, const char* suffix) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

#endif // FILE_SINKS_H

