#ifndef INPUT_SOURCE_H
#define INPUT_SOURCE_H

#include <cstdint>
#include <string>
#include <memory>
#include <chrono> // added for steady_clock::time_point
#include "config/config_common.h"
// ---- Common types -----------------------------------------------------------

//enum class PixelFormat : uint8_t { NV12, RGB24, BGR24 };

struct FrameView {
  PixelFormat fmt{PixelFormat::NV12};
  int width{0};
  int height{0};
  int stride0{0};        // e.g., Y stride for NV12
  int stride1{0};        // e.g., UV stride for NV12
  uint8_t* plane0{nullptr};
  uint8_t* plane1{nullptr};       // nullptr when single-planar
  int64_t pts_ns{0};              // capture PTS in nanoseconds
  // V6.2.3.2: Original camera frame dimensions (before resize to model input size)
  // Used for de-letterboxing YOLOX detections back to camera coordinate space
  int orig_width{0};     // Original camera width (e.g., 1280)
  int orig_height{0};    // Original camera height (e.g., 720)
  //ColorLayout fmt{ColorLayout::NV12};
};

struct CaptureFrame {
  int width{0};
  int height{0};
  PixelFormat fmt{PixelFormat::BGR24};   // <-- was BGR, fix to BGR24
  std::vector<uint8_t> data;             // width*height*3 for *24 formats
  std::chrono::steady_clock::time_point ts;
};

// --- Per-source option structs ---
struct UsbOptions {
  int width{640};
  int height{480};
  int fps{30};
};

enum class HealthStatus : uint8_t {
  Starting,
  Ok,
  Degraded,
  Disconnected,
  Error
};

struct HealthInfo {
  HealthStatus status{HealthStatus::Starting};
  bool connected{false};
  uint64_t frames_ok{0};
  int64_t last_ok_ns{0};   // steady_clock::now() in ns
};

enum class InputType : uint8_t {
  Unknown,
  RTSP,
  USB,
  File
};

enum class FetchStatus : uint8_t {
  Ok,
  Timeout,
  Error,
  Broken
};

// ---- Interface --------------------------------------------------------------

class IInputSource {
public:
  virtual ~IInputSource() = default;

  // Identify the concrete source
  virtual InputType type() const noexcept = 0;

  // Resource lifecycle
  // open(): create underlying handles/pipeline but do NOT start capture
  virtual bool open()  noexcept = 0;
  // start(): begin capture/decoding threads; idempotent
  virtual bool start() noexcept = 0;
  // stop(): stop capture threads; safe to call multiple times
  virtual void stop()  noexcept = 0;
  // close(): free resources fully
  virtual void close() noexcept = 0;

  // Non-blocking fetch of the latest frame (or next frame).
  // Implementations SHOULD avoid allocation and fill 'out' to point at
  // internal buffers valid until the next tryFetch() call.
  virtual FetchStatus tryFetch(FrameView& out) noexcept = 0;

  virtual FetchStatus fetch(FrameView& out, int timeout_ms) noexcept = 0;

  // Lightweight snapshot; must not block for long.
  virtual HealthInfo getHealth() const noexcept = 0;

  // Non-copyable
  IInputSource(const IInputSource&) = delete;
  IInputSource& operator=(const IInputSource&) = delete;

protected:
  IInputSource() = default;
};

#endif // INPUT_SOURCE_H
