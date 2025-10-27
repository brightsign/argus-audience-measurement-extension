#ifndef STAGE_TIMER_H
#define STAGE_TIMER_H

#include <cstdint>
#include <chrono>
#include "metrics/metrics_types.h"

// Manual timer per stage (no allocations)
class StageTimer {
public:
  void start() noexcept { t0_ = Clock::now(); }
  // Returns elapsed ns
  int64_t stop() noexcept {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0_).count();
    last_ns_ = ns;
    return ns;
  }
  int64_t last_ns() const noexcept { return last_ns_; }
private:
  using Clock = std::chrono::steady_clock;
  Clock::time_point t0_{Clock::now()};
  int64_t last_ns_{0};
};

// RAII scope timer that writes into an int64_t* on destruction
class ScopeTimer {
public:
  explicit ScopeTimer(int64_t* out_ns) noexcept : out_(out_ns), t0_(Clock::now()) {}
  ~ScopeTimer() { if (out_) *out_ = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-t0_).count(); }
private:
  using Clock = std::chrono::steady_clock;
  int64_t* out_{};
  Clock::time_point t0_;
};

// Helper macro to time a scope into a variable
#define METRIC_SCOPE_NS(varname) int64_t varname##_ns = 0; ScopeTimer varname##_tm(&varname##_ns)

#endif // STAGE_TIMER_H

