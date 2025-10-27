#include "pipeline/capture_worker.h"

bool CaptureWorker::start() noexcept {
  if (!src_ || !out_q_) return false;
  if (th_.joinable()) return true;
  stop_.store(false, std::memory_order_release);
  th_ = std::thread(&CaptureWorker::run, this);
  return true;
}
void CaptureWorker::stop() noexcept {
  stop_.store(true, std::memory_order_release);
  if (th_.joinable()) th_.join();
}

void CaptureWorker::run() noexcept {
  if (src_) { src_->open(); src_->start(); }
  while (!stop_.load(std::memory_order_acquire)) {
    FrameView fv{};
    auto st = src_->tryFetch(fv);
    if (st == FetchStatus::Ok) {
      RawFrame rf{};
      rf.fmt = PixFmt::NV12; rf.width = fv.width; rf.height=fv.height;
      rf.stride0=fv.stride0; rf.stride1=fv.stride1; rf.plane0=fv.plane0; rf.plane1=fv.plane1;
      rf.pts_ns=fv.pts_ns; rf.seq=seq_++;
      out_q_->push(rf);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  if (src_) { src_->stop(); src_->close(); }
}

