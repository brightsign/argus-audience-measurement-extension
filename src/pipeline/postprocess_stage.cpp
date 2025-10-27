#include "pipeline/postprocess_stage.h"
#include <algorithm>

bool PostprocessStage::start() noexcept {
  if (!params_ || !in_q_ || !cb_) return false;
  if (th_.joinable()) return true;
  stop_.store(false, std::memory_order_release);
  th_ = std::thread(&PostprocessStage::run, this);
  return true;
}
void PostprocessStage::stop() noexcept {
  stop_.store(true, std::memory_order_release);
  if (th_.joinable()) th_.join();
}

void PostprocessStage::nms(std::vector<Detection>&, float, bool) noexcept { /* TODO */ }
void PostprocessStage::compute_gaze(Track&) noexcept { /* TODO */ }

void PostprocessStage::run() noexcept {
  InferenceOut io{};
  while (!stop_.load(std::memory_order_acquire)) {
    if (!in_q_->pop(io)) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
    PipelineResult res{};
    res.seq = io.seq; res.pts_ns = io.pts_ns;
    // TODO: build tracks from detections/landmarks with thresholds and NMS
    cb_(res);
  }
}

