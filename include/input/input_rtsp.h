#ifndef INPUT_RTSP_H
#define INPUT_RTSP_H

#include "input/input_source.h"
#include <memory>
#include <string>

struct RtspOptions {
  int latency_ms{200};          // pipeline/network latency target
  bool tcp{true};               // use TCP interleaved if true
  int drop_on_lag_ms{500};      // drop late frames to keep up
  int reconnect_backoff_ms{500};
  int reconnect_backoff_max_ms{5000};
};

class RtspInputSource final : public IInputSource {
public:
  explicit RtspInputSource(std::string url, RtspOptions opts = {});
  ~RtspInputSource() override;

  InputType type() const noexcept override { return InputType::RTSP; }
  bool open()  noexcept override;
  bool start() noexcept override;
  void stop()  noexcept override;
  void close() noexcept override;
  FetchStatus tryFetch(FrameView& out) noexcept override;
  FetchStatus fetch(FrameView& out, int timeout_ms) noexcept override; // added
  HealthInfo getHealth() const noexcept override;

private:
  struct Impl;                       // hides GStreamer types, threads, etc.
  std::unique_ptr<Impl> p_;
};

#endif // INPUT_RTSP_H
