#ifndef MODEL_RUNNER_H
#define MODEL_RUNNER_H

#include <cstdint>
#include <memory>
#include "model_spec.h"

// Lightweight frame view (aligns with your InputSource::FrameView)
struct FrameView {
  // NOTE: keep this in sync with your input_source.h version
  int width{0}, height{0};
  int stride0{0}, stride1{0};
  uint8_t* plane0{nullptr};
  uint8_t* plane1{nullptr};
  int64_t pts_ns{0};
  ColorLayout fmt{ColorLayout::NV12};
};

// Minimal generic outputs (non-owning where possible)
struct Detection {
  float x0, y0, x1, y1;
  float score;
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
