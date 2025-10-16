#ifndef OUTPUT_TYPES_H
#define OUTPUT_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

// Reuse your existing types from earlier modules when you include them:
// - PipelineResult (from pipeline_types.h)
// - TelemetrySnapshot (from metrics_types.h)

enum class OutputKind : uint8_t { Result=0, Telemetry=1, Log=2, Blob=3 };
enum class OutputFormat : uint8_t { JSON=0, Binary=1, Text=2 };

struct LogRecord {
  // millisecond-resolution is enough for logs; payload is short text
  int64_t ts_ns{0};
  const char* tag{nullptr};     // e.g., "pipeline", "rtsp", "npu"
  const char* message{nullptr}; // short, non-owning string literal or fmt’d buffer
  uint8_t level{1};             // 0=dbg,1=info,2=warn,3=err
};

// Optional: RTSP/transport stats you may choose to publish
struct RtspStats {
  uint32_t rtp_lost{0};
  uint32_t rtp_jitter_ms{0};
  uint32_t rtp_out_of_order{0};
  uint32_t decoder_drops{0};
  int64_t  window_ns{0};
};

// Frame annotation style (debug)
struct BoxStyle { uint8_t r{0}, g{255}, b{0}, a{255}; int thickness{2}; };
struct GazeStyle{ uint8_t r{255}, g{0},  b{0}, a{255}; int thickness{2}; int length_px{40}; };

struct AnnotationSpec {
  bool draw_boxes{true};
  bool draw_landmarks{true};
  bool draw_gaze{true};
  BoxStyle  box{};
  GazeStyle gaze{};
};

#endif // OUTPUT_TYPES_H

