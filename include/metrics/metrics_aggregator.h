#ifndef METRICS_AGGREGATOR_H
#define METRICS_AGGREGATOR_H

#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include "metrics/metrics_types.h"

// Lightweight per-stage histogram with uniform bins
class UniformHistogram {
public:
  explicit UniformHistogram(const HistogramConfig& cfg) noexcept
  : cfg_(cfg), counts_(cfg.bins, 0) {}

  void add_sample_ms(float ms) noexcept {
    if (cfg_.bins == 0 || cfg_.max <= cfg_.min) return;
    float clamped = (ms < cfg_.min) ? cfg_.min : (ms >= cfg_.max ? (cfg_.max - 1e-6f) : ms);
    float width = (cfg_.max - cfg_.min) / cfg_.bins;
    uint16_t idx = static_cast<uint16_t>((clamped - cfg_.min) / width);
    if (idx >= cfg_.bins) idx = cfg_.bins - 1;
    counts_[idx] += 1;
  }

  const std::vector<uint32_t>& bins() const noexcept { return counts_; }
  HistogramConfig cfg() const noexcept { return cfg_; }
  void reset() noexcept { std::fill(counts_.begin(), counts_.end(), 0); }

private:
  HistogramConfig cfg_{};
  std::vector<uint32_t> counts_;
};

// Sliding FPS over a ring of frame times
class SlidingFps {
public:
  explicit SlidingFps(const SlidingFpsConfig& c) noexcept : cfg_(c) {
    times_ns_.assign(cfg_.window_frames ? cfg_.window_frames : 1, 0);
  }

  // push a new frame duration (ns) and compute stats
  void push(int64_t dt_ns) noexcept {
    if (times_ns_.empty()) return;
    // subtract old
    sum_ns_ -= times_ns_[idx_];
    // add new
    times_ns_[idx_] = dt_ns;
    sum_ns_ += dt_ns;

    // compute instantaneous
    inst_fps_ = dt_ns > 0 ? (1e9f / static_cast<float>(dt_ns)) : 0.0f;

    // move idx
    idx_ = (idx_ + 1) % times_ns_.size();
    if (filled_ < times_ns_.size()) filled_++;

    // recompute min/max/avg cheaply
    float avg_ns = (filled_ ? (static_cast<float>(sum_ns_) / filled_) : 0.0f);
    avg_fps_ = (avg_ns > 0) ? (1e9f / avg_ns) : 0.0f;

    // simple min/max recompute (window small, acceptable)
    float minf = 1e9f, maxf = 0.0f;
    for (size_t i = 0; i < filled_; ++i) {
      auto ns = times_ns_[i];
      float fps = ns > 0 ? (1e9f / ns) : 0.0f;
      if (fps < minf) minf = fps;
      if (fps > maxf) maxf = fps;
    }
    min_fps_ = (filled_ ? minf : 0.0f);
    max_fps_ = (filled_ ? maxf : 0.0f);
  }

  FpsStats snapshot() const noexcept {
    return FpsStats{inst_fps_, avg_fps_, min_fps_, max_fps_};
  }

  void reset() noexcept {
    std::fill(times_ns_.begin(), times_ns_.end(), 0);
    sum_ns_ = 0; idx_ = 0; filled_ = 0;
    inst_fps_ = avg_fps_ = min_fps_ = max_fps_ = 0.0f;
  }

private:
  SlidingFpsConfig cfg_{};
  std::vector<int64_t> times_ns_;
  size_t idx_{0};
  size_t filled_{0};
  int64_t sum_ns_{0};
  float inst_fps_{0}, avg_fps_{0}, min_fps_{0}, max_fps_{0};
};

// Aggregates metrics for one pipeline instance (one camera/source)
class MetricsAggregator {
public:
  struct Config {
    SlidingFpsConfig fps{};
    HistogramConfig  hist{0.0f, 50.0f, 25};
    StageDebugFlags  debug{};
  };

  explicit MetricsAggregator(const Config& cfg) noexcept
  : cfg_(cfg),
    fps_(cfg.fps),
    hists_(static_cast<size_t>(Stage::COUNT), UniformHistogram(cfg.hist)) {}

  // Record a stage timing (ns). Optionally fill histogram.
  void add_stage_time(Stage s, int64_t ns) noexcept {
    auto i = static_cast<size_t>(s);
    if (i >= static_cast<size_t>(Stage::COUNT)) return;
    auto ms = static_cast<float>(ns) / 1.0e6f;
    if (cfg_.debug.enable_histograms) hists_[i].add_sample_ms(ms);
    stats_[i].count += 1;
    stats_[i].total_ns += static_cast<uint64_t>(ns);
    stats_[i].last_ms = static_cast<uint32_t>(ms + 0.5f);
  }

  // Throughput tracking: push full-frame time (ns)
  void add_frame_time(int64_t frame_ns) noexcept { fps_.push(frame_ns); frames_ok_++; }

  // Error & recovery counters
  void add_error(FaultCode code) noexcept {
    errors_.total_errors += 1;
    errors_.by_code[static_cast<size_t>(code) & 0x0F] += 1;
  }
  void add_recovery() noexcept { errors_.total_recoveries += 1; }

  // Optional: system stats (fill from orchestrator)
  void set_system_util(float cpu_load, float mem_used_frac, float npu_util) noexcept {
    cpu_load_ = cpu_load; mem_used_frac_ = mem_used_frac; npu_util_ = npu_util;
  }

  TelemetrySnapshot snapshot(int64_t now_ns) const noexcept {
    TelemetrySnapshot t{};
    t.ts_ns = now_ns;
    for (size_t i=0;i<static_cast<size_t>(Stage::COUNT);++i) {
      t.stage_avg_ms[i] = stats_[i].count ? (static_cast<float>(stats_[i].total_ns) / stats_[i].count / 1.0e6f) : 0.0f;
    }
    t.fps = fps_.snapshot();
    t.frames_ok = frames_ok_;
    t.errors = errors_;
    t.cpu_load = cpu_load_;
    t.mem_used_frac = mem_used_frac_;
    t.npu_util = npu_util_;
    return t;
  }

  // Access per-stage histogram (read-only)
  const UniformHistogram& histogram(Stage s) const noexcept {
    return hists_[static_cast<size_t>(s)];
  }

  // Reset accumulators (e.g., on pipeline restart)
  void reset() noexcept {
    for (auto& s : stats_) { s = {}; }
    for (auto& h : hists_) { h.reset(); }
    fps_.reset();
    frames_ok_ = 0;
    errors_ = {};
  }

  const Config& config() const noexcept { return cfg_; }
  void set_debug_flags(const StageDebugFlags& dbg) noexcept { cfg_.debug = dbg; }

private:
  Config cfg_{};
  StageTimingStats stats_[static_cast<size_t>(Stage::COUNT)]{};
  std::vector<UniformHistogram> hists_;
  SlidingFps fps_;
  uint64_t frames_ok_{0};
  ErrorCounters errors_{};
  float cpu_load_{0.0f}, mem_used_frac_{0.0f}, npu_util_{0.0f};
};

#endif // METRICS_AGGREGATOR_H

