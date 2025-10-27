#include "pipeline/inference_stage.h"

bool InferenceStage::start() noexcept {
  if (!runner_ || !in_q_ || !out_q_) return false;
  if (th_.joinable()) return true;
  stop_.store(false, std::memory_order_release);
  th_ = std::thread(&InferenceStage::run, this);
  return true;
}
void InferenceStage::stop() noexcept {
  stop_.store(true, std::memory_order_release);
  if (th_.joinable()) th_.join();
}

void InferenceStage::run() noexcept {
  PreprocFrame pf{};
  while (!stop_.load(std::memory_order_acquire)) {
    if (!in_q_->pop(pf)) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
    InferenceOutputs outs{};
    runner_->infer(FrameView{}, outs);
    InferenceOut io{};
    io.dets = outs.dets; io.num_dets=outs.num_dets; io.lms=outs.lms; io.num_lms=outs.num_lms;
    io.pts_ns = pf.pts_ns; io.seq = pf.seq;
    out_q_->push(io);
  }
}

