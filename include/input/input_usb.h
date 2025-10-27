#pragma once
#include <atomic>
#include <string>
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

private:
  std::string      device_;
  cv::VideoCapture cap_;
  cv::Mat          last_frame_; // store last captured frame for FrameView
  int              pref_w_{0};
  int              pref_h_{0};
  double           pref_fps_{0.0};
  std::vector<uint8_t> scratch_bgr_;

  std::atomic<bool>     broken_{false};
  std::atomic<bool>     connected_{false};
  std::atomic<uint64_t> frames_ok_{0};
  std::atomic<int64_t>  last_ok_ns_{0};
};
