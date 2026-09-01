#include "output/async_publisher.h"
#include <cstring>

AsyncPublisher::AsyncPublisher(PublisherPtr inner) noexcept
: inner_(std::move(inner)), cfg_{} {}

AsyncPublisher::AsyncPublisher(PublisherPtr inner, const Config& cfg) noexcept
: inner_(std::move(inner)), cfg_(cfg) {}

bool AsyncPublisher::start() noexcept {
  if (!inner_) return false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (th_.joinable()) return true;  // already running
    q_.clear();
  }
  stop_.store(false, std::memory_order_release);
  if (!inner_->start()) return false;
  th_ = std::thread(&AsyncPublisher::run, this);
  return true;
}
void AsyncPublisher::stop() noexcept {
  stop_.store(true, std::memory_order_release);
  cv_.notify_all();
  if (th_.joinable()) th_.join();
  if (inner_) inner_->stop();
}

bool AsyncPublisher::enqueue(Msg&& m) noexcept {
  const size_t cap = cfg_.queue_capacity ? cfg_.queue_capacity : 64;
  try {
    {
      std::lock_guard<std::mutex> lk(mu_);
      // Drop-old: never block the producer on a slow sink.
      while (q_.size() >= cap) q_.pop_front();
      q_.push_back(std::move(m));
    }
    cv_.notify_one();
  } catch (...) {
    // Allocation failure: drop this message rather than throw from noexcept.
    return false;
  }
  return true;
}

bool AsyncPublisher::publish_result(const PipelineResult& r) noexcept { return enqueue(Msg{r}); }
bool AsyncPublisher::publish_telemetry(const TelemetrySnapshot& t) noexcept { return enqueue(Msg{t}); }
bool AsyncPublisher::publish_log(const LogRecord& rec) noexcept { return enqueue(Msg{rec}); }
bool AsyncPublisher::publish_blob(OutputFormat fmt, const void* data, size_t len) noexcept {
  MsgBlob b{fmt, data, len}; return enqueue(Msg{b});
}

void AsyncPublisher::run() noexcept {
  for (;;) {
    Msg m;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this]{ return stop_.load(std::memory_order_acquire) || !q_.empty(); });
      if (stop_.load(std::memory_order_acquire) && q_.empty()) return;
      if (q_.empty()) continue;
      m = std::move(q_.front());
      q_.pop_front();
    }

    // Deliver to the wrapped sink outside the lock so a slow sink never
    // blocks producers.
    std::visit([this](auto&& item){
      using T = std::decay_t<decltype(item)>;
      if constexpr (std::is_same_v<T, PipelineResult>) inner_->publish_result(item);
      else if constexpr (std::is_same_v<T, TelemetrySnapshot>) inner_->publish_telemetry(item);
      else if constexpr (std::is_same_v<T, LogRecord>) inner_->publish_log(item);
      else { inner_->publish_blob(item.fmt, item.data, item.len); }
    }, m);
  }
}

