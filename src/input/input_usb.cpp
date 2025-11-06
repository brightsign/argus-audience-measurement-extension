#include "input/input_usb.h"
#include "metrics/log_global.h"
#include <chrono>
#include <thread>
#include <cstring>



using SteadyClock = std::chrono::steady_clock; // was clock_t alias conflicting with system clock_t
static inline int64_t to_ns(SteadyClock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

UsbInputSource::UsbInputSource(const std::string& device)
: device_(device) {}

UsbInputSource::UsbInputSource(const std::string& device, const UsbOptions& opts)
: device_(device) {
  if (opts.width > 0) pref_w_ = opts.width;
  if (opts.height > 0) pref_h_ = opts.height;
  if (opts.fps > 0) pref_fps_ = opts.fps;
}

UsbInputSource::~UsbInputSource() { close(); }

bool UsbInputSource::open() noexcept {
  close();
  broken_.store(false);
  stopping_.store(false);
  frames_ok_.store(0);
  connected_.store(false);
  last_ok_ns_.store(to_ns(SteadyClock::now()));

  // Accept "0" style tokens too
  std::string dev = device_;
  if (dev.empty() || dev == "usb_camera") dev = "/dev/video0";
  int backend = cv::CAP_V4L2;

  if (!cap_.open(dev, backend)) {
    LG_ERROR("usb cv::VideoCapture open failed: %s", dev.c_str());
    broken_.store(true);
    return false;
  }

  cap_.set(cv::CAP_PROP_FOURCC,
             cv::VideoWriter::fourcc('M','J','P','G'));
  LG_INFO("pref_fps_ %.1f", pref_fps_);
  if (pref_w_ > 0)  cap_.set(cv::CAP_PROP_FRAME_WIDTH,  pref_w_);
  if (pref_h_ > 0)  cap_.set(cv::CAP_PROP_FRAME_HEIGHT, pref_h_);
  if (pref_fps_> 0) cap_.set(cv::CAP_PROP_FPS,          pref_fps_);

  connected_.store(true);
  LG_INFO("usb opened %s (w=%d h=%d fps=%.1f)",
          dev.c_str(),
          (int)cap_.get(cv::CAP_PROP_FRAME_WIDTH),
          (int)cap_.get(cv::CAP_PROP_FRAME_HEIGHT),
          cap_.get(cv::CAP_PROP_FPS));
  
  return true;
}

bool UsbInputSource::start() noexcept {
  // For simple V4L2/OpenCV case, open() already started capture.
  // Return true if device is opened and not marked broken.
  return cap_.isOpened() && !broken_.load();
}

void UsbInputSource::stop() noexcept {
  // Nothing special for OpenCV VideoCapture; we keep it opened until close().
}

void UsbInputSource::close() noexcept {
  try {
    if (cap_.isOpened()) cap_.release();
  } catch (...) {}
  connected_.store(false);
}

FetchStatus UsbInputSource::tryFetch(FrameView& out) noexcept {
  // If we're being told to stop, exit immediately
  if (stopping_.load(std::memory_order_relaxed)) {
    LG_INFO("tryFetch:exiting immediately due to stopping flag\n");
    return FetchStatus::Broken;
  }

  if (!cap_.isOpened() || broken_.load(std::memory_order_relaxed)) {
    LG_INFO("tryFetch:cap not open or broken (isOpened=%d, broken=%d)\n", 
            cap_.isOpened(), broken_.load(std::memory_order_relaxed));
    return FetchStatus::Broken;
  }

  cv::Mat bgr;
  auto fetch_start = std::chrono::steady_clock::now();

  // We'll poll up to ~50ms total.
  // 10 attempts * 5ms sleep = ~50ms worst case.
  // This prevents blocking indefinitely on stubborn V4L2 drivers.
  for (int attempt = 0; attempt < 10; ++attempt) {
    // Check stopping flag frequently
    if (stopping_.load(std::memory_order_relaxed)) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - fetch_start).count();
      LG_WARN("tryFetch:stopping flag detected in loop at attempt %d (elapsed=%ldms)\n",
              attempt, elapsed);
      return FetchStatus::Broken;
    }
    if (!cap_.isOpened()) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - fetch_start).count();
      LG_WARN("tryFetch:cap closed in loop at attempt %d (elapsed=%ldms)\n",
              attempt, elapsed);
      return FetchStatus::Broken;
    }

    // Try to grab a frame (non-blocking or fast)
    bool got = cap_.grab();
    if (got) {
      // Now retrieve the grabbed frame (fast copy from buffer)
      if (!cap_.retrieve(bgr) || bgr.empty()) {
        // Something went wrong pulling the data
        broken_.store(true, std::memory_order_relaxed);
        return FetchStatus::Broken;
      }

      // Success path: package it, update health, return Ok
      const size_t need_sz = size_t(bgr.cols) * size_t(bgr.rows) * 3;
      if (scratch_bgr_.size() != need_sz) {
        scratch_bgr_.resize(need_sz);
      }
      std::memcpy(scratch_bgr_.data(), bgr.data, need_sz);

      const int64_t now_ns_val =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()
          ).count();

      // Return full resolution frame without pre-scaling
      // Let each inference worker (face/yolo) resize as needed
      out.fmt     = PixelFormat::BGR24;
      out.width   = bgr.cols;
      out.height  = bgr.rows;
      out.stride0 = bgr.cols * 3;
      out.stride1 = 0;
      out.plane0  = scratch_bgr_.data();
      out.plane1  = nullptr;
      out.pts_ns  = now_ns_val;

      frames_ok_.fetch_add(1, std::memory_order_relaxed);
      last_ok_ns_.store(now_ns_val, std::memory_order_relaxed);
      connected_.store(true, std::memory_order_relaxed);

      return FetchStatus::Ok;
    }

    // If grab() failed, don't block forever. Sleep a bit, check stop again, try again.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // If we get here after 10 attempts (~50ms), camera isn't giving frames right now.
  // This is typically temporary (between frames or buffer empty).
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - fetch_start).count();
  LG_INFO("tryFetch:timeout after ~50ms (actual=%ldms), returning Timeout\n", elapsed);
  return FetchStatus::Timeout;
}


FetchStatus UsbInputSource::fetch(FrameView& out, int timeout_ms) noexcept {
  auto start = SteadyClock::now();
  while (true) {
    auto st = tryFetch(out);
    if (st == FetchStatus::Ok || st == FetchStatus::Error) return st;
    if (timeout_ms <= 0) return st;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - start).count();
    if (elapsed_ms >= timeout_ms) return FetchStatus::Timeout;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void UsbInputSource::setPreferredResolution(int w, int h) {
  pref_w_ = w; pref_h_ = h;
}
void UsbInputSource::setPreferredFps(double fps) {
  pref_fps_ = fps;
}

HealthInfo UsbInputSource::getHealth() const noexcept {
  HealthInfo h{};
  h.frames_ok = frames_ok_.load(std::memory_order_relaxed);
  h.last_ok_ns = last_ok_ns_.load(std::memory_order_relaxed);
  h.connected = cap_.isOpened();
  if (broken_.load()) h.status = HealthStatus::Error;
  else if (!h.connected) h.status = HealthStatus::Disconnected;
  else h.status = HealthStatus::Ok;
  return h;
}
