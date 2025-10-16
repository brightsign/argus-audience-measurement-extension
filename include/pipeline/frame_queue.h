#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Single-producer/single-consumer ring with drop-old push policy.
template <typename T>
class SpscDropOld {
public:
  explicit SpscDropOld(size_t capacity) noexcept
  : cap_(capacity ? capacity : 1), buf_(cap_) {}

  // Producer: overwrite oldest when full (drop-old)
  void push(const T& v) noexcept {
    size_t w = write_.load(std::memory_order_relaxed);
    size_t r = read_.load(std::memory_order_acquire);
    size_t next = (w + 1) % cap_;
    if (next == r) { // full, drop oldest by advancing read
      read_.store((r + 1) % cap_, std::memory_order_release);
    }
    buf_[w] = v;
    write_.store(next, std::memory_order_release);
  }

  // Consumer: returns false if empty
  bool pop(T& out) noexcept {
    size_t r = read_.load(std::memory_order_relaxed);
    size_t w = write_.load(std::memory_order_acquire);
    if (r == w) return false; // empty
    out = buf_[r];
    read_.store((r + 1) % cap_, std::memory_order_release);
    return true;
  }

  bool empty() const noexcept { return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire); }
  size_t capacity() const noexcept { return cap_; }

  void reset() noexcept {
    read_.store(0, std::memory_order_relaxed);
    write_.store(0, std::memory_order_relaxed);
  }

private:
  const size_t cap_;
  std::vector<T> buf_;
  std::atomic<size_t> read_{0}, write_{0};
};

#endif // FRAME_QUEUE_H
