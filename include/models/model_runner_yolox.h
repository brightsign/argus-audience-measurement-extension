#ifndef MODEL_RUNNER_YOLOX_H
#define MODEL_RUNNER_YOLOX_H

#include "models/model_runner.h"
#include <memory>
#include <vector>

/**
 * RKNNYoloXRunner: YOLOX object detector on Rockchip NPU.
 * 
 * Runs on NPU core 1 (configured during load()).
 * Input: 640x640 BGR24
 * Output: Multi-scale detections (person, objects, etc.)
 * 
 * Will be implemented with same pattern as RKNNRetinafaceRunner.
 */
class RKNNYoloXRunner final : public IModelRunner {
public:
    RKNNYoloXRunner() noexcept;
    ~RKNNYoloXRunner() override;

    // Not copyable/movable
    RKNNYoloXRunner(const RKNNYoloXRunner&) = delete;
    RKNNYoloXRunner& operator=(const RKNNYoloXRunner&) = delete;

    const ModelSpec& spec() const noexcept override { return spec_; }
    
    // Load model from disk, set NPU core affinity from spec_.npu_core
    bool load(const ModelSpec& spec) noexcept override;
    
    // Reshape input dimensions (may be no-op for fixed YOLOX)
    bool reshape(int new_w, int new_h) noexcept override;
    
    // Run one inference frame -> detections
    bool infer(const FrameView& in, InferenceOutputs& out) noexcept override;
    
    // Free RKNN resources
    void unload() noexcept override;

    // Profiling timers (nanoseconds)
    int64_t last_pre_ns()   const noexcept override { return pre_ns_; }
    int64_t last_infer_ns() const noexcept override { return infer_ns_; }
    int64_t last_post_ns()  const noexcept override { return post_ns_; }

    // Accessors for detection data (for visualization)
    const Detection* get_detections() const noexcept { return dets_.data(); }
    int get_detection_count() const noexcept { return static_cast<int>(dets_.size()); }

private:
    struct Impl;
    std::unique_ptr<Impl> p_;

    ModelSpec spec_;
    std::vector<Detection> dets_;   // per-frame scratch
    std::vector<Landmarks> lms_;    // placeholder (YOLOX doesn't produce landmarks)

    int64_t pre_ns_{0};
    int64_t infer_ns_{0};
    int64_t post_ns_{0};
};

#endif // MODEL_RUNNER_YOLOX_H
