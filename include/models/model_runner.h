#ifndef MODEL_RUNNER_H
#define MODEL_RUNNER_H

#include <cstdint>
#include <memory>
#include "config/model_spec.h"
#include "input/input_source.h"


// Minimal generic outputs (non-owning where possible)
struct Detection {
  float x0{0.0f}, y0{0.0f}, x1{0.0f}, y1{0.0f};
  float score{0.0f};
  int   class_id{-1};
};

struct Landmarks { float pts[10]; }; // (x,y)*5 for faces

struct InferenceOutputs {
  const Detection* dets{nullptr};
  int num_dets{0};
  const Landmarks* lms{nullptr};
  int num_lms{0};
};

// Abstract interface for any model runner
class IModelRunner {
public:
  virtual ~IModelRunner() = default;

  virtual const ModelSpec& spec() const noexcept = 0;

  // Prepare network resources
  virtual bool load(const ModelSpec& spec) noexcept = 0;

  // Optional reshape if upstream changes size; return false if unsupported
  virtual bool reshape(int new_w, int new_h) noexcept = 0;

  // Run one inference. Returns false on fatal error.
  virtual bool infer(const FrameView& in, InferenceOutputs& out) noexcept = 0;

  // Optional: release resources early
  virtual void unload() noexcept = 0;

  // Timing hooks (ns) for profiling
  virtual int64_t last_pre_ns()   const noexcept = 0;
  virtual int64_t last_infer_ns() const noexcept = 0;
  virtual int64_t last_post_ns()  const noexcept = 0;
};

using IModelRunnerPtr = std::unique_ptr<IModelRunner>;

#endif // MODEL_RUNNER_H
