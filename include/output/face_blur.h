#pragma once

#include <opencv2/opencv.hpp>
#include <vector>

namespace output {

enum class BlurMethod {
    PIXELATE,
    GAUSSIAN
};

struct BlurConfig {
    bool enabled = false;
    BlurMethod method = BlurMethod::PIXELATE;
    int intensity = 12;  // block_size for pixelate (4-32), kernel_size for gaussian (31-99)
};

/**
 * Apply blur to face regions in a frame for privacy protection.
 *
 * @param frame The image to modify (in-place)
 * @param face_bboxes Vector of face bounding boxes (x0, y0, x1, y1 format)
 * @param config Blur configuration settings
 */
void blur_faces(cv::Mat& frame,
                const std::vector<cv::Rect>& face_bboxes,
                const BlurConfig& config);

/**
 * Convert Detection-style bbox (x0,y0,x1,y1 floats) to cv::Rect
 */
inline cv::Rect detection_to_rect(float x0, float y0, float x1, float y1) {
    return cv::Rect(
        static_cast<int>(x0),
        static_cast<int>(y0),
        static_cast<int>(x1 - x0),
        static_cast<int>(y1 - y0)
    );
}

} // namespace output
