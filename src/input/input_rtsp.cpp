#include "input/input_rtsp.h"
#include <atomic>
#include <thread>
#include <chrono>

struct RtspInputSource::Impl {
  std::string url;
  RtspOptions opts;
  std::atomic<bool> opened{false};
  std::atomic<bool> running{false};
  HealthInfo health{};
};

RtspInputSource::RtspInputSource(std::string url, RtspOptions opts)
: p_(new Impl{std::move(url), opts}) {}
RtspInputSource::~RtspInputSource() = default;

bool RtspInputSource::open() noexcept { p_->opened.store(true); return true; }
bool RtspInputSource::start() noexcept { if (!p_->opened) return false; p_->running.store(true); return true; }
void RtspInputSource::stop() noexcept { p_->running.store(false); }
void RtspInputSource::close() noexcept { p_->opened.store(false); }

FetchStatus RtspInputSource::tryFetch(FrameView& out) noexcept {
  (void)out;
  if (!p_->running.load()) return FetchStatus::Timeout;
  return FetchStatus::Timeout; // TODO integrate pipeline
}

FetchStatus RtspInputSource::fetch(FrameView& out, int timeout_ms) noexcept {
  // Simple polling using tryFetch until timeout
  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto st = tryFetch(out);
    if (st == FetchStatus::Ok || st == FetchStatus::Error) return st;
    if (timeout_ms <= 0) return st; // Timeout or initial
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (elapsed >= timeout_ms) return FetchStatus::Timeout;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

HealthInfo RtspInputSource::getHealth() const noexcept { return p_->health; }

