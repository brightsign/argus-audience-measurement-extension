#include "pipeline/preprocess_stage.h"

bool PreprocessStage::start() noexcept {
  if (!rm_ || !model_ || !in_q_ || !out_q_) return false;
  if (th_.joinable()) return true;
  stop_.store(false, std::memory_order_release);
  th_ = std::thread(&PreprocessStage::run, this);
  return true;
}
void PreprocessStage::stop() noexcept {
  stop_.store(true, std::memory_order_release);
  if (th_.joinable()) th_.join();
}

void PreprocessStage::run() noexcept {
  RawFrame rf{};
  while (!stop_.load(std::memory_order_acquire)) {
    if (!in_q_->pop(rf)) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
    PreprocFrame pf{};
    pf.pts_ns = rf.pts_ns; pf.seq = rf.seq;
    // Minimal stub: just forward metadata; real impl should RGA letterbox/convert/normalize
    out_q_->push(pf);
  }
}

