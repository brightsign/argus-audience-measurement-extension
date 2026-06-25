#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <opencv2/opencv.hpp>
#include "input/input_source.h"

#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

class UsbInputSource final : public IInputSource {
public:
  explicit UsbInputSource(const std::string& device);
  UsbInputSource(const std::string& device, const UsbOptions& opts); // new overload
  ~UsbInputSource() override;

  InputType type() const noexcept override { return InputType::USB; }
  bool open() noexcept override;                   // match base
  bool start() noexcept override;                  // added
  void stop() noexcept override;                   // added
  void close() noexcept override;
  FetchStatus tryFetch(FrameView& out) noexcept override; // changed CaptureFrame->FrameView
  FetchStatus fetch(FrameView& out, int timeout_ms) noexcept override; // added blocking fetch

  bool broken() const noexcept { return broken_.load(); } // removed override
  HealthInfo getHealth() const noexcept override;

  void setPreferredResolution(int w, int h);
  void setPreferredFps(double fps);

  // Called by orchestrator before stopping worker to unblock any blocking read()
  void request_stop() noexcept {
    // Raw V4L2 capture uses a short select() timeout in tryFetch(), so simply
    // setting this flag lets the worker thread exit promptly without a risky
    // cross-thread close of the V4L2 file descriptor.
    stopping_.store(true, std::memory_order_release);
  }

private:
  // Raw V4L2 mmap capture. We bypass cv::VideoCapture because its open() issues
  // VIDIOC_STREAMON internally, locking the UVC frame rate before CAP_PROP_FPS
  // can take effect. Driving V4L2 directly lets us issue VIDIOC_S_PARM (fps)
  // BEFORE VIDIOC_STREAMON, so the camera honours the requested rate.
  void cleanup() noexcept;

  std::string      device_;
  int              fd_{-1};
  bool             streaming_{false};
  uint32_t         pixfmt_{0};   // V4L2 fourcc the driver actually negotiated
  int              cap_w_{0};    // negotiated capture width
  int              cap_h_{0};    // negotiated capture height
  std::vector<void*>  bufs_;
  std::vector<size_t> buf_lens_;
  int              pref_w_{0};
  int              pref_h_{0};
  double           pref_fps_{0.0};
  std::vector<uint8_t> scratch_bgr_;

  std::atomic<bool>     stopping_{false};    // NEW: for graceful shutdown
  std::atomic<bool>     broken_{false};
  std::atomic<bool>     connected_{false};
  std::atomic<uint64_t> frames_ok_{0};
  std::atomic<int64_t>  last_ok_ns_{0};
};
