#pragma once

#include <opencv2/core.hpp>
#include <cstdint>
#include "output/face_blur.h"

// Forward declarations
class IModelRunner;
struct FusionResults;

namespace visualization {

// Draw bounding boxes, landmarks, and labels on RGB frame
// Saves debug JPEG every Nth frame to /tmp/output.jpg
// V6.2.3.2: Added orig_width/orig_height to scale coordinates for visualization
// V6.2.3.5.7: Added second_runner to draw detections from multiple models on same frame
// V7.0.2: Added fusion_output to read synchronized detection results instead of stale cache
// V7.2: Added blur_config to apply person blur before drawing boxes
void process_inference_results(
    IModelRunner* runner,
    cv::Mat& rgb_mat,
    uint32_t& debug_frame_idx,
    int orig_width = 0,
    int orig_height = 0,
    IModelRunner* second_runner = nullptr,
    FusionResults* fusion_output = nullptr,
    const output::BlurConfig& blur_config = output::BlurConfig{},
    bool flip_h = false) noexcept;

// Save frame as JPEG every Nth frame (controlled by frame_idx % 3)
void save_debug_jpg(
    const cv::Mat& visFrame,
    const char* path,
    uint32_t frame_idx) noexcept;

} // namespace visualization
