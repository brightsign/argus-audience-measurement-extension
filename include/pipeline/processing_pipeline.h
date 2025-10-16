#ifndef PROCESSING_PIPELINE_H
#define PROCESSING_PIPELINE_H

#include <memory>
#include <atomic>
#include "pipeline_types.h"
#include "frame_queue.h"
#include "capture_worker.h"
#include "preprocess_stage.h"
#include "inference_stage.h"
#include "postprocess_stage.h"
#include "input_factory.h"
#include "model_factory.h"
#include "resource_manager.h"
#include "processing_params.h"
#include "configuration.h"       // AppConfig if you use it

struct PipelineBuildConfig {
  // Queues
  size_t qA_capacity{2};   // Capture → Preprocess (1–2 frames)
  size_t qB_capacity{1};   // Preprocess → Inference (1 frame)

  // Stages/config
  InputConfig       input{};
  ModelSpec         model{};
  ProcessingParams  proc{};
  PreprocessConfig  pre_cfg{};
  ResourceConfig    res_cfg{};
};

class ProcessingPipeline {
public:
  explicit ProcessingPipeline(const PipelineBuildConfig& cfg,
                              ResultCallback cb) noexcept
  : cfg_(cfg), cb_(std::move(cb)),
    qA_(cfg.qA_capacity), qB_(cfg.qB_capacity) {}

  ~ProcessingPipeline() { stop(); }

  // Build/start/stop
  bool start() noexcept;
  void stop() noexcept;

  // Accessors
  SpscDropOld<RawFrame>*       queue_capture()   noexcept { return &qA_; }
  SpscDropOld<PreprocFrame>*   queue_inference() noexcept { return &qB_; }

private:
  bool build() noexcept;
  void teardown() noexcept;

  PipelineBuildConfig cfg_;
  ResultCallback cb_;

  // Queues
  SpscDropOld<RawFrame>     qA_; // Capture → Preprocess
  SpscDropOld<PreprocFrame> qB_; // Preprocess → Inference

  // Resources
  std::unique_ptr<ResourceManager> rm_;

  // Stages
  std::unique_ptr<IInputSource>   input_;
  std::unique_ptr<CaptureWorker>  capture_;
  std::unique_ptr<PreprocessStage> pre_;
  std::unique_ptr<IModelRunner>    runner_;
  std::unique_ptr<InferenceStage>  infer_;
  std::unique_ptr<PostprocessStage> post_;
};

#endif // PROCESSING_PIPELINE_H

