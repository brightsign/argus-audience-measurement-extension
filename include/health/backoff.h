#ifndef BACKOFF_H
#define BACKOFF_H

#include <cstdint>
#include <algorithm>

struct BackoffPolicy {
  int base_ms{250};
  int max_ms{5000};
  float factor{2.0f};
  int jitter_ms{50};       // +/- jitter to decorrelate
};

struct BackoffState {
  BackoffPolicy policy{};
  int current_ms{0};
  uint32_t failures{0};
  int64_t last_attempt_ns{0};
  int64_t last_success_ns{0};

  // call when a failure happens; returns the next delay (ms)
  int onFailure(int64_t now_ns) noexcept {
    failures++;
    if (current_ms <= 0) current_ms = policy.base_ms;
    else {
      float next = current_ms * policy.factor;
      current_ms = std::min(policy.max_ms, static_cast<int>(next));
    }
    // simple deterministic jitter
    if (policy.jitter_ms > 0) {
      uint32_t h = 2654435761u * failures;
      int j = (int)(h % (policy.jitter_ms * 2 + 1)) - policy.jitter_ms;
      current_ms = std::max(0, current_ms + j);
    }
    last_attempt_ns = now_ns;
    return current_ms;
  }

  // call when frames are flowing again
  void onSuccess(int64_t now_ns) noexcept {
    last_success_ns = now_ns;
    // keep current_ms (useful for visibility), but reset counters so next failure starts at base
    failures = 0;
  }

  void reset() noexcept {
    current_ms = 0;
    failures = 0;
    last_attempt_ns = 0;
    last_success_ns = 0;
  }
};

#endif // BACKOFF_H

