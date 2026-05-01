#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <atomic>
#include <string>
#include <mutex>

// Full includes instead of forward declarations
#include "models/model_runner.h"
#include "pipeline/shared_frame.h"
#include "pipeline/frame_mailbox.h"
#include "pipeline/pipeline_types.h"
#include "output/frame_writer.h"
#include "output/face_blur.h"
#include "tracking/tracker.h"

// Type alias for the fusion output structure (defined in orchestrator.h)
struct FusionResults {
    std::mutex m;
    std::vector<Detection> face_dets;
    std::vector<Landmarks> face_lms;
    uint64_t face_seq{0};
    std::vector<Detection> yolo_dets;
    uint64_t yolo_seq{0};
    // Person tracks with stable IDs and uniform/vest classification results
    std::vector<TrackedBox> tracks;
};

namespace inference_worker {

// Generic inference worker configuration
struct WorkerConfig {
    int skip_frames;           // Process every Nth frame (1 = all, 2 = every other, etc)
    int model_input_width;     // Model input size
    int model_input_height;
    std::string model_name;    // For logging
    output::BlurConfig blur_config;  // Person blur configuration (applied before drawing boxes)
    bool flip_horizontal{false};     // Flip output frame left-right (mirror correction)
};

// Run inference loop for a single model
// - Consumes frames from frame_mailbox
// - Runs model inference with frame skipping
// - Draws overlays and saves debug JPEGs
// - Stores results in fusion output
// - Respects stop_flag for clean shutdown
// V6.2.3.5.7: Added second_runner to draw detections from multiple models on same frame
void run_inference_loop(
    IModelRunner* runner,
    std::shared_ptr<FrameMailbox> frame_mailbox,
    FusionResults* fusion_output,
    IFrameWriter* frame_writer,
    const WorkerConfig& config,
    const std::atomic<bool>& stop_flag,
    IModelRunner* second_runner = nullptr) noexcept;

} // namespace inference_worker
