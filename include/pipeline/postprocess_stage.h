#ifndef POSTPROCESS_STAGE_H
#define POSTPROCESS_STAGE_H

#include <atomic>
#include <thread>
#include <functional>
#include "pipeline/pipeline_types.h"
#include "pipeline/frame_queue.h"
#include "config/processing_params.h"     // thresholds + NMS from config

using ResultCallback = std::function<void(const PipelineResult&)>;

class PostprocessStage {
public:
  PostprocessStage(const ProcessingParams* params,
                   SpscDropOld<InferenceOut>* in_q,
                   ResultCallback cb) noexcept
  : params_(params), in_q_(in_q), cb_(std::move(cb)) {}

  ~PostprocessStage() { stop(); }

  bool start() noexcept;
  void stop() noexcept;

private:
  void run() noexcept;
  void nms(std::vector<Detection>& dets, float iou, bool class_agnostic) noexcept;
  void compute_gaze(Track& t) noexcept;

  const ProcessingParams* params_{nullptr};
  SpscDropOld<InferenceOut>* in_q_{nullptr};
  ResultCallback cb_{};

  std::thread th_;
  std::atomic<bool> stop_{false};
};

#endif // POSTPROCESS_STAGE_H

