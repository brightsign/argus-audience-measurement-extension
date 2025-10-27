#ifndef PREPROCESS_STAGE_H
#define PREPROCESS_STAGE_H

#include <atomic>
#include <thread>
#include <memory>
#include "pipeline/pipeline_types.h"
#include "pipeline/frame_queue.h"
#include "resources/resource_manager.h"    // RGA + pools + tensors
#include "config/model_spec.h"          // input size/layout + normalization

struct PreprocessConfig {
  bool  keep_aspect{true};       // letterbox vs stretch
  uint8_t fill_y{0};             // letterbox padding
  uint8_t fill_uv{128};
  bool  to_tensor{true};         // write directly into model input
};

class PreprocessStage {
public:
  PreprocessStage(ResourceManager* rm,
                  const ModelSpec* model,
                  const PreprocessConfig& cfg,
                  SpscDropOld<RawFrame>* in_q,
                  SpscDropOld<PreprocFrame>* out_q) noexcept
  : rm_(rm), model_(model), cfg_(cfg), in_q_(in_q), out_q_(out_q) {}

  ~PreprocessStage() { stop(); }

  bool start() noexcept;
  void stop() noexcept;

private:
  void run() noexcept;
  bool do_convert_letterbox_normalize(const RawFrame& in, PreprocFrame& out) noexcept;

  ResourceManager* rm_{nullptr};
  const ModelSpec* model_{nullptr};
  PreprocessConfig cfg_{};
  SpscDropOld<RawFrame>* in_q_{nullptr};
  SpscDropOld<PreprocFrame>* out_q_{nullptr};

  std::thread th_;
  std::atomic<bool> stop_{false};
};

#endif // PREPROCESS_STAGE_H

