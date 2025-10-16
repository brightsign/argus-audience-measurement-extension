#ifndef MODEL_RUNNER_RETINAFACE_H
#define MODEL_RUNNER_RETINAFACE_H

#include "model_runner.h"
#include <memory>
#include <vector>

// RKNN types are hidden in the Impl to avoid leaking heavy headers here.
class RKNNRetinafaceRunner final : public IModelRunner {
public:
  RKNNRetinafaceRunner() noexcept;
  ~RKNNRetinafaceRunner() override;

  const ModelSpec& spec() const noexcept override { return spec_; }
  bool load(const ModelSpec& spec) noexcept override;
  bool reshape(int new_w, int new_h) noexcept override;
  bool infer(const FrameView& in, InferenceOutputs& out) noexcept override;
  void unload() noexcept override;

  int64_t last_pre_ns()   const noexcept override { return pre_ns_; }
  int64_t last_infer_ns() const noexcept override { return infer_ns_; }
  int64_t last_post_ns()  const noexcept override { return post_ns_; }

private:
  struct Impl;                    // defined in .cpp; holds rknn_app_context_t etc.
  std::unique_ptr<Impl> p_;
  ModelSpec spec_;

  // scratch ownership for outputs (valid until next infer())
  std::vector<Detection> dets_;
  std::vector<Landmarks> lms_;

  int64_t pre_ns_{0}, infer_ns_{0}, post_ns_{0};
};

#endif // MODEL_RUNNER_RETINAFACE_H
