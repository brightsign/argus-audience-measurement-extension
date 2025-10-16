#ifndef HEALTH_MANAGER_H
#define HEALTH_MANAGER_H

#include <cstdint>
#include <atomic>
#include <cstring>
#include "backoff.h"

// Keep enums tiny & explicit, to match your inference.cpp style
enum class SourceKind : uint8_t { Unknown=0, RTSP=1, USB=2, File=3 };
enum class Severity  : uint8_t { Info=0, Warning=1, Error=2, Critical=3 };
enum class FaultCode : uint8_t {
  None=0,
  // RTSP / GStreamer
  GstError,
  GstEos,
  AppSinkStarvation,
  RtpJitterHigh,
  // USB
  DeviceGone,
  RepeatedReadFailures,
  // Generic
  NoFrames,
  DecodeFailure
};

// Compact snapshot for logging/telemetry
struct HealthSnapshot {
  SourceKind kind{SourceKind::Unknown};
  Severity   worst_recent{Severity::Info};
  FaultCode  last_fault{FaultCode::None};
  uint32_t   consecutive_failures{0};

  // counters
  uint64_t frames_ok{0};
  uint64_t starvations{0};
  uint64_t bus_errors{0};
  uint64_t bus_eos{0};
  uint64_t read_failures{0};

  // times (steady_clock ns)
  int64_t last_ok_ns{0};
  int64_t last_fault_ns{0};
  int64_t last_recovery_ns{0};

  // current backoff suggestion
  int backoff_ms{0};
};


class HealthManager {
public:
  explicit HealthManager(SourceKind k) noexcept : kind_(k) {}

  // ---- RTSP events ----
  void onBusError(int64_t now_ns, const char* msg = nullptr) noexcept {
    (void)msg; bus_errors_.fetch_add(1, std::memory_order_relaxed);
    set_fault(now_ns, FaultCode::GstError, Severity::Error);
  }
  void onBusEos(int64_t now_ns) noexcept {
    bus_eos_.fetch_add(1, std::memory_order_relaxed);
    set_fault(now_ns, FaultCode::GstEos, Severity::Warning);
  }
  void onAppsinkStarvation(int64_t now_ns) noexcept {
    starvations_.fetch_add(1, std::memory_order_relaxed);
    set_fault(now_ns, FaultCode::AppSinkStarvation, Severity::Error);
  }
  void onRtpJitterHigh(int64_t now_ns) noexcept {
    set_fault(now_ns, FaultCode::RtpJitterHigh, Severity::Warning);
  }

  // ---- USB events ----
  void onUsbDeviceGone(int64_t now_ns) noexcept {
    set_fault(now_ns, FaultCode::DeviceGone, Severity::Error);
  }
  void onUsbReadFailure(int64_t now_ns) noexcept {
    read_failures_.fetch_add(1, std::memory_order_relaxed);
    set_fault(now_ns, FaultCode::RepeatedReadFailures, Severity::Error);
  }

  // ---- Generic events ----
  void onNoFrames(int64_t now_ns) noexcept {
    set_fault(now_ns, FaultCode::NoFrames, Severity::Warning);
  }
  void onDecodeFailure(int64_t now_ns) noexcept {
    set_fault(now_ns, FaultCode::DecodeFailure, Severity::Error);
  }

  // Call when a good frame successfully passes through to inference
  void onFrameOk(int64_t now_ns) noexcept {
    frames_ok_.fetch_add(1, std::memory_order_relaxed);
    last_ok_ns_.store(now_ns, std::memory_order_relaxed);
    consecutive_failures_.store(0, std::memory_order_relaxed);
    worst_recent_.store(static_cast<uint8_t>(Severity::Info), std::memory_order_relaxed);
    // success contributes to recovering backoff state
    backoff_.onSuccess(now_ns);
    // if someone is polling the 'broken' flag, they can clear it on success externally
  }

  // External code (your appsink thread / capture loop) can flip this when it considers the source broken.
  void markBroken() noexcept { broken_.store(true, std::memory_order_release); }
  void clearBroken() noexcept { broken_.store(false, std::memory_order_release); }
  bool isBroken() const noexcept { return broken_.load(std::memory_order_acquire); }

  // Backoff configuration & computation
  void setBackoffPolicy(const BackoffPolicy& p) noexcept { backoff_.policy = p; }
  // Call after a failure to get the next delay to respect before reacquire/restart
  int nextBackoffMs(int64_t now_ns) noexcept {
    int ms = backoff_.onFailure(now_ns);
    last_backoff_ms_.store(ms, std::memory_order_relaxed);
    return ms;
  }

  // Useful for your acquire loop: record that a recovery attempt just occurred
  void noteRecoveryAttempt(int64_t now_ns) noexcept {
    last_recovery_ns_.store(now_ns, std::memory_order_relaxed);
  }

  // Log-friendly snapshot (cheap)
  HealthSnapshot snapshot() const noexcept {
    HealthSnapshot s;
    s.kind = kind_;
    s.worst_recent = static_cast<Severity>(worst_recent_.load(std::memory_order_relaxed));
    s.last_fault   = last_fault_.load(std::memory_order_relaxed);
    s.consecutive_failures = consecutive_failures_.load(std::memory_order_relaxed);
    s.frames_ok    = frames_ok_.load(std::memory_order_relaxed);
    s.starvations  = starvations_.load(std::memory_order_relaxed);
    s.bus_errors   = bus_errors_.load(std::memory_order_relaxed);
    s.bus_eos      = bus_eos_.load(std::memory_order_relaxed);
    s.read_failures= read_failures_.load(std::memory_order_relaxed);
    s.last_ok_ns   = last_ok_ns_.load(std::memory_order_relaxed);
    s.last_fault_ns= last_fault_ns_.load(std::memory_order_relaxed);
    s.last_recovery_ns = last_recovery_ns_.load(std::memory_order_relaxed);
    s.backoff_ms   = last_backoff_ms_.load(std::memory_order_relaxed);
    return s;
  }

private:
  void set_fault(int64_t now_ns, FaultCode code, Severity sev) noexcept {
    last_fault_.store(code, std::memory_order_relaxed);
    last_fault_ns_.store(now_ns, std::memory_order_relaxed);
    broken_.store(true, std::memory_order_release);
    // track worst seen severity since last good frame
    uint8_t cur = worst_recent_.load(std::memory_order_relaxed);
    if (static_cast<uint8_t>(sev) > cur) {
      worst_recent_.store(static_cast<uint8_t>(sev), std::memory_order_relaxed);
    }
    consecutive_failures_.fetch_add(1, std::memory_order_relaxed);
  }

  // identity
  SourceKind kind_{SourceKind::Unknown};

  // atomics/counters
  std::atomic<bool> broken_{false};
  std::atomic<uint8_t> worst_recent_{static_cast<uint8_t>(Severity::Info)};
  std::atomic<FaultCode> last_fault_{FaultCode::None};
  std::atomic<uint32_t> consecutive_failures_{0};

  std::atomic<uint64_t> frames_ok_{0};
  std::atomic<uint64_t> starvations_{0};
  std::atomic<uint64_t> bus_errors_{0};
  std::atomic<uint64_t> bus_eos_{0};
  std::atomic<uint64_t> read_failures_{0};

  std::atomic<int64_t> last_ok_ns_{0};
  std::atomic<int64_t> last_fault_ns_{0};
  std::atomic<int64_t> last_recovery_ns_{0};
  std::atomic<int>     last_backoff_ms_{0};

  BackoffState backoff_{};
};

#endif // HEALTH_MANAGER_H
