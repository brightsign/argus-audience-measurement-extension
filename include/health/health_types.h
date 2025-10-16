#ifndef HEALTH_TYPES_H
#define HEALTH_TYPES_H

#include <cstdint>
#include <string>

// Keep aligned with your existing HealthStatus from input_source.h if included elsewhere
enum class HealthStatus : uint8_t {
  Starting,
  Ok,
  Degraded,
  Disconnected,
  Error
};

enum class StageType : uint8_t {
  Unknown = 0,
  InputRTSP,
  InputUSB,
  InputFile,
  Preprocess,
  Inference,
  Postprocess,
  Output
};

enum class Severity : uint8_t {
  Info = 0,
  Warning,
  Error,
  Critical
};

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

struct StageKey {
  StageType type{StageType::Unknown};
  uint32_t  id{0};              // e.g., camera index or pipeline instance
  const char* name{nullptr};    // optional human label, not owning
};

// Snapshot of rolling metrics a stage may report (all optional; zeros mean "not reported")
struct StageMetrics {
  // Frame/call counters
  uint64_t frames_in{0};
  uint64_t frames_out{0};
  uint64_t errors{0};
  uint64_t starvations{0};

  // Timing
  int64_t  last_ok_ns{0};       // steady_clock::now() in ns of last good output
  int64_t  last_input_ns{0};    // when input last arrived

  // RTSP specifics
  int      rtp_jitter_ms{0};    // recent jitter
  int      clock_drift_ppm{0};  // estimated drift

  // USB specifics
  uint32_t repeated_read_failures{0};

  // Bus errors surfaced from GStreamer (count since start)
  uint32_t bus_errors{0};
  uint32_t bus_eos{0};
};

// A concrete fault event (edge-triggered)
struct FaultEvent {
  StageKey  stage{};
  FaultCode code{FaultCode::None};
  Severity  severity{Severity::Warning};
  int64_t   ts_ns{0};           // when detected
  const char* detail{nullptr};  // optional non-owning string literal or short msg
};

// Manager’s normalized view of a stage’s current health
struct StageHealth {
  HealthStatus status{HealthStatus::Starting};
  Severity     worst_recent{Severity::Info};
  uint32_t     consecutive_failures{0};
  int64_t      last_fault_ns{0};
  int64_t      last_recovery_ns{0};
};

// What the manager suggests the orchestrator should do next
enum class RecoveryAction : uint8_t {
  None = 0,          // keep going; informational only
  ReduceLoad,        // e.g., drop fps, reduce resolution
  RestartStage,      // stop/start the affected stage
  RecreatePipeline,  // rebuild the pipeline graph for that stage
  ReacquireSource,   // close/open capture (RTSP reconnect / reopen USB)
  SwitchToBackup,    // fail over to a backup source
  NotifyOnly         // escalate alert without action
};

// Advice bundle
struct RecoveryAdvice {
  RecoveryAction action{RecoveryAction::None};
  int            backoff_ms{0};     // how long to wait before attempting the action
  Severity       severity{Severity::Info};
  FaultCode      cause{FaultCode::None};
};

#endif // HEALTH_TYPES_H

