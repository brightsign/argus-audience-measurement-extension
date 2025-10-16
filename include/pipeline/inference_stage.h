#ifndef INFERENCE_STAGE_H
#define INFERENCE_STAGE_H

#include <atomic>
#include <thread>
#include <memory>
#include "pipeline_types.h"
#include "frame_queue.h"
#include "model_runner.h"        // IModelRunner

class InferenceStage {
public:
  InferenceStage(std::unique_ptr<IModelRunner> runner,
                 SpscDropOld<PreprocFrame>* in_q,
                 SpscDropOld<InferenceOut>* out_q) noexcept
  : runner_(std::move(runner)), in_q_(in_q), out_q_(out_q) {}

  ~InferenceStage() { stop(); }

  bool start() noexcept;
  void stop() noexcept;

private:
  void run() noexcept;

  std::unique_ptr<IModelRunner> runner_;
  SpscDropOld<PreprocFrame>* in_q_{nullptr};
  SpscDropOld<InferenceOut>* out_q_{nullptr};

  std::thread th_;
  std::atomic<bool> stop_{false};
};

#endif // INFERENCE_STAGE_H

