#ifndef METRICS_TYPES_H
#define METRICS_TYPES_H

#include <cstdint>
#include <array>
#include <string>

// Optional forward-declare to avoid pulling health headers here.
// If you include your health headers first, the real enum will be used.
enum class Severity : uint8_t { Info=0, Warning=1, Error=2, Critical=3 };

// Specific fault codes we may detect/receive
enum class FaultCode : uint16_t {
  None = 0,
  // RTSP / GStreamer
  GstError,
  GstEos,
  AppSinkStarvation,
  RtpJitterHigh,
  ClockDriftHigh,

  // USB / V4L2
  DeviceGone,
  RepeatedReadFailures,

  // Generic pipeline
  BackPressure,
  StageTimeout,
  NoFrames,
  DecodeFailure,
  InferenceTimeout
};


enum class Stage : uint8_t {
  Capture=0, Convert, Preprocess, Inference, Postprocess, Publish, COUNT
};

struct SlidingFpsConfig {
  // Number of frames to smooth over (ring buffer size). Keep small for embedded.
  uint16_t window_frames{120}; // ~4 sec @30 FPS
};

struct HistogramConfig {
  // Uniform histogram: [min, max) split into 'bins' buckets.
  float   min{0.0f};
  float   max{50.0f};   // milliseconds
  uint16_t bins{25};    // 2 ms per bin default
};

struct StageTimingStats {
  // ns-resolution aggregates since last reset
  uint64_t count{0};
  uint64_t total_ns{0};
  uint32_t last_ms{0};        // last duration in ms (rounded)
  // lightweight histogram (kept in counts; bins sized by HistogramConfig)
  // Stored externally in aggregator to avoid dynamic size here.
};

struct ErrorCounters {
  uint64_t total_errors{0};
  uint64_t total_recoveries{0};
  uint64_t by_code[16]{};     // small fixed map for common faults; index by (code % 16)
};

struct FpsStats {
  float inst_fps{0.0f};       // instantaneous (1 / last frame time)
  float avg_fps{0.0f};        // sliding-window average
  float min_fps{0.0f};        // sliding-window min
  float max_fps{0.0f};        // sliding-window max
};

struct StageDebugFlags {
  bool enable_profiling{false};
  bool enable_histograms{false};
  bool enable_verbose{false};
};

// System telemetry snapshot (cheap POD)
struct TelemetrySnapshot {
  // Time (steady_clock ns) when snapshot made
  int64_t ts_ns{0};

  // Per-stage timings (ms average since last reset)
  float stage_avg_ms[static_cast<size_t>(Stage::COUNT)]{};

  // Throughput
  FpsStats fps{};

  // Health-ish indicators
  uint64_t frames_ok{0};
  ErrorCounters errors{};

  // Optional headroom indicators (0..1)
  float cpu_load{0.0f};
  float mem_used_frac{0.0f};
  float npu_util{0.0f};
};

#endif // METRICS_TYPES_H

