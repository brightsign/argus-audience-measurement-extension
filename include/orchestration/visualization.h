#pragma once

#include <opencv2/core.hpp>
#include <cstdint>

// Forward declarations
class IModelRunner;

namespace visualization {

// Draw bounding boxes, landmarks, and labels on RGB frame
// Saves debug JPEG every Nth frame to /tmp/output.jpg
void process_inference_results(
    IModelRunner* runner,
    cv::Mat& rgb_mat,
    uint32_t& debug_frame_idx) noexcept;

// Save frame as JPEG every Nth frame (controlled by frame_idx % 3)
void save_debug_jpg(
    const cv::Mat& visFrame,
    const char* path,
    uint32_t frame_idx) noexcept;

} // namespace visualization
