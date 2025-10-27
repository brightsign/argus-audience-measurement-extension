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

#if 0
FetchStatus UsbInputSource::tryFetch(FrameView& out) noexcept {
  if (!cap_.isOpened() || broken_.load()) return FetchStatus::Error;
  
  // OpenCV's cap_.read() can block indefinitely on slow V4L2 drivers
  // Use grab() which is non-blocking, then retrieve() on success
  // If no frame is ready after grab(), return Timeout (not Error)
  // This prevents marking the source as broken during normal buffering delays
  
  cv::Mat bgr;
  
  // Try to grab the next frame (non-blocking)
  // grab() returns false if no frame is ready yet
  if (!cap_.grab()) {
    // No frame available at this moment; will retry soon
    return FetchStatus::Timeout;
  }
  
  // We grabbed a frame; now retrieve it (should be fast)
  if (!cap_.retrieve(bgr) || bgr.empty()) {
    // Frame was grabbed but retrieval failed or empty
    broken_.store(true);
    return FetchStatus::Error;
  }
  
  // Success: store frame and populate output
  last_frame_ = bgr;
  out.width  = bgr.cols;
  out.height = bgr.rows;
  out.fmt    = PixelFormat::BGR24;
  out.plane0 = last_frame_.data;
  out.plane1 = nullptr;
  out.stride0 = static_cast<int>(bgr.step); // bytes per row
  out.stride1 = 0;
  out.pts_ns = to_ns(SteadyClock::now());
  frames_ok_.fetch_add(1, std::memory_order_relaxed);
  last_ok_ns_.store(out.pts_ns, std::memory_order_relaxed);
  return FetchStatus::Ok;
}
#endif

FetchStatus UsbInputSource::tryFetch(FrameView& out) noexcept {
    if (!cap_.isOpened() || broken_.load()) {
        return FetchStatus::Broken;
    }

    cv::Mat bgr;
    if (!cap_.read(bgr) || bgr.empty()) {
        broken_.store(true);
        return FetchStatus::Broken;
    }

    // We will keep an internal scratch buffer so the data stays alive
    // until the next call (store it as a member: std::vector<uint8_t> scratch_bgr_;).
    if (scratch_bgr_.size() != size_t(bgr.cols * bgr.rows * 3)) {
        scratch_bgr_.resize(size_t(bgr.cols * bgr.rows * 3));
    }
    std::memcpy(scratch_bgr_.data(), bgr.data, scratch_bgr_.size());

    out.fmt     = PixelFormat::BGR24;
    out.width   = bgr.cols;
    out.height  = bgr.rows;
    out.stride0 = bgr.cols * 3;
    out.stride1 = 0;
    out.plane0  = scratch_bgr_.data();
    out.plane1  = nullptr;
    out.pts_ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

    // NOTE: if your FrameView struct does NOT have seq or pts_ns, we'll handle that below.

    frames_ok_.fetch_add(1, std::memory_order_relaxed);
    last_ok_ns_.store(out.pts_ns, std::memory_order_relaxed);
    connected_.store(true, std::memory_order_relaxed);

    return FetchStatus::Ok;
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
