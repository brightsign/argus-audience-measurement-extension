#include "output/async_publisher.h"
#include <cstring>

AsyncPublisher::AsyncPublisher(PublisherPtr inner) noexcept
: inner_(std::move(inner)), cfg_{} {}

AsyncPublisher::AsyncPublisher(PublisherPtr inner, const Config& cfg) noexcept
: inner_(std::move(inner)), cfg_(cfg) {}

bool AsyncPublisher::start() noexcept {
  if (!inner_) return false;
  if (!q_.empty()) return true;
  q_.assign(cfg_.queue_capacity ? cfg_.queue_capacity : 64, Msg{});
  r_.store(0); w_.store(0);
  stop_.store(false, std::memory_order_release);
  if (!inner_->start()) return false;
  th_ = std::thread(&AsyncPublisher::run, this);
  return true;
}
void AsyncPublisher::stop() noexcept {
  stop_.store(true, std::memory_order_release);
  if (th_.joinable()) th_.join();
  if (inner_) inner_->stop();
}

bool AsyncPublisher::enqueue(Msg&& m) noexcept {
  const size_t cap = q_.size();
  size_t w = w_.load(std::memory_order_relaxed);
  size_t r = r_.load(std::memory_order_acquire);
  size_t next = (w + 1) % cap;
  if (next == r) { // full -> drop-old
    r = (r + 1) % cap;
    r_.store(r, std::memory_order_release);
  }
  q_[w] = std::move(m);
  w_.store(next, std::memory_order_release);
  return true;
}

bool AsyncPublisher::publish_result(const PipelineResult& r) noexcept { return enqueue(Msg{r}); }
bool AsyncPublisher::publish_telemetry(const TelemetrySnapshot& t) noexcept { return enqueue(Msg{t}); }
bool AsyncPublisher::publish_log(const LogRecord& rec) noexcept { return enqueue(Msg{rec}); }
bool AsyncPublisher::publish_blob(OutputFormat fmt, const void* data, size_t len) noexcept {
  MsgBlob b{fmt, data, len}; return enqueue(Msg{b});
}

void AsyncPublisher::run() noexcept {
  const size_t cap = q_.size();
  while (!stop_.load(std::memory_order_acquire)) {
    size_t r = r_.load(std::memory_order_relaxed);
    size_t w = w_.load(std::memory_order_acquire);
    if (r == w) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
    Msg m = std::move(q_[r]);
    r_.store((r+1)%cap, std::memory_order_release);

    std::visit([this](auto&& item){
      using T = std::decay_t<decltype(item)>;
      if constexpr (std::is_same_v<T, PipelineResult>) inner_->publish_result(item);
      else if constexpr (std::is_same_v<T, TelemetrySnapshot>) inner_->publish_telemetry(item);
      else if constexpr (std::is_same_v<T, LogRecord>) inner_->publish_log(item);
      else { inner_->publish_blob(item.fmt, item.data, item.len); }
    }, m);
  }
}

