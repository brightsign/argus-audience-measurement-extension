#ifndef CAPTURE_WORKER_H
#define CAPTURE_WORKER_H

#include <atomic>
#include <thread>
#include <memory>
#include "pipeline/pipeline_types.h"
#include "input/input_source.h" 
#include "pipeline/frame_queue.h"

class CaptureWorker {
public:
  CaptureWorker(std::unique_ptr<IInputSource> src,
                SpscDropOld<RawFrame>* out_q,
                uint64_t seq_start = 1) noexcept
  : src_(std::move(src)), out_q_(out_q), seq_(seq_start) {}

  ~CaptureWorker() { stop(); }

  bool start() noexcept;
  void stop() noexcept;

private:
  void run() noexcept;

  std::unique_ptr<IInputSource> src_;
  SpscDropOld<RawFrame>* out_q_{nullptr};
  std::thread th_;
  std::atomic<bool> stop_{false};
  uint64_t seq_{1};
};

#endif // CAPTURE_WORKER_H

