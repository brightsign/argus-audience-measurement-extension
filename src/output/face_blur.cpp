#include "output/face_blur.h"
#include <algorithm>

namespace output {

void blur_faces(cv::Mat& frame,
                const std::vector<cv::Rect>& face_bboxes,
                const BlurConfig& config) {
    if (!config.enabled || face_bboxes.empty() || frame.empty()) {
        return;
    }

    const cv::Rect frame_bounds(0, 0, frame.cols, frame.rows);

    for (const auto& bbox : face_bboxes) {
        // Clamp bounding box to frame dimensions
        cv::Rect safe_bbox = bbox & frame_bounds;
        if (safe_bbox.area() <= 0) {
            continue;
        }

        cv::Mat roi = frame(safe_bbox);

        switch (config.method) {
            case BlurMethod::PIXELATE: {
                // Clamp block size to reasonable range
                int block_size = std::clamp(config.intensity, 4, 32);

                // Downsample then upsample with nearest-neighbor
                cv::Mat small;
                cv::resize(roi, small, cv::Size(block_size, block_size),
                          0, 0, cv::INTER_LINEAR);
                cv::resize(small, roi, roi.size(),
                          0, 0, cv::INTER_NEAREST);
                break;
            }
            case BlurMethod::GAUSSIAN: {
                // Kernel size must be odd and reasonably large for privacy
                int ksize = config.intensity | 1;  // Ensure odd
                ksize = std::clamp(ksize, 31, 99);
                cv::GaussianBlur(roi, roi, cv::Size(ksize, ksize), 0);
                break;
            }
        }
    }
}

} // namespace output
