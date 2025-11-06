#include "orchestration/visualization.h"
#include "models/model_runner.h"
#include "models/model_runner_retinaface.h"
#include "models/model_runner_yolox.h"
#include "attention.h"
#include "metrics/log_global.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <cstdio>

namespace visualization {

void save_debug_jpg(
    const cv::Mat& visFrame,
    const char* path,
    uint32_t frame_idx) noexcept
{
    // DISABLED: JPEG saving causes significant I/O overhead and NPU load increase
    // Keeping function for API compatibility but no-op
    (void)visFrame;
    (void)path;
    (void)frame_idx;
}

static void draw_face_detections(
    cv::Mat& drawMat,
    const RKNNRetinafaceRunner* retinaface_runner) noexcept
{
    const retinaface_result* result =
        static_cast<const retinaface_result*>(
            retinaface_runner->get_last_result());

    if (!result || result->count == 0) {
        return;
    }

    int attending_total = 0;

    for (int i = 0; i < result->count; ++i) {
        const auto& obj = result->object[i];

        // choose box color: green if looking, red otherwise
        bool attending = face_is_looking_at_us(obj);
        if (attending) {
            attending_total++;
        }

        // BGR format: (B, G, R)
        cv::Scalar box_color = attending
            ? cv::Scalar(0, 255, 0)      // green in BGR
            : cv::Scalar(0, 0, 255);     // red in BGR

        // draw face bbox
        const auto& box = obj.box;
        cv::rectangle(
            drawMat,
            cv::Point((int)box.left,  (int)box.top),
            cv::Point((int)box.right, (int)box.bottom),
            box_color,
            2
        );

        // draw 5 landmarks
        for (int lm = 0; lm < 5; ++lm) {
            int lx = (int)obj.ponit[lm].x;
            int ly = (int)obj.ponit[lm].y;

            // eye landmarks cyan, others yellow
            cv::Scalar lm_color = (lm < 2)
                ? cv::Scalar(0, 255, 255)    // cyan
                : cv::Scalar(255, 255, 0);   // yellow

            cv::circle(
                drawMat,
                cv::Point(lx, ly),
                2,
                lm_color,
                2,
                cv::LINE_AA
            );
        }

        // put "attn" label if attending
        if (attending) {
            cv::putText(drawMat,
                        "ATTN",
                        cv::Point((int)box.left,
                                  (int)box.top - 4),
                        cv::FONT_HERSHEY_SIMPLEX,
                        0.4,
                        box_color,
                        1,
                        cv::LINE_AA);
        }
    }

    #ifdef DEBUG_LOGS
    LG_INFO("overlay: faces=%d attending=%d",
            result->count,
            attending_total);
    #endif
}

static void draw_yolo_detections(
    cv::Mat& drawMat,
    const RKNNYoloXRunner* yolox_runner) noexcept
{
    const Detection* dets = yolox_runner->get_detections();
    int det_count = yolox_runner->get_detection_count();

    if (!dets || det_count == 0) {
        return;
    }

    // Color palette for different class IDs
    const int max_colors = 10;
    const cv::Scalar colors[max_colors] = {
        cv::Scalar(0, 255, 255),      // Cyan
        cv::Scalar(0, 0, 255),        // Red
        cv::Scalar(0, 255, 0),        // Green
        cv::Scalar(255, 0, 255),      // Magenta
        cv::Scalar(0, 165, 255),      // Orange
        cv::Scalar(255, 255, 0),      // Yellow
        cv::Scalar(255, 0, 0),        // Blue
        cv::Scalar(255, 0, 127),      // Purple
        cv::Scalar(255, 255, 0),      // Cyan (repeat)
        cv::Scalar(0, 0, 0),          // Black
    };

    for (int i = 0; i < det_count; ++i) {
        const auto& det = dets[i];

        // Select color based on class_id
        cv::Scalar box_color = colors[det.class_id % max_colors];

        // Draw bounding box
        int x0 = (int)det.x0;
        int y0 = (int)det.y0;
        int x1 = (int)det.x1;
        int y1 = (int)det.y1;

        cv::rectangle(
            drawMat,
            cv::Point(x0, y0),
            cv::Point(x1, y1),
            box_color,
            2
        );

        // Draw score and class_id label
        char label[64];
        snprintf(label, sizeof(label), "cls=%d score=%.2f", 
                 det.class_id, det.score);

        cv::putText(
            drawMat,
            label,
            cv::Point(x0, y0 - 5),
            cv::FONT_HERSHEY_SIMPLEX,
            0.4,
            box_color,
            1,
            cv::LINE_AA
        );
    }

    #ifdef DEBUG_LOGS
    LG_INFO("overlay: YOLO objects=%d", det_count);
    #endif
}

void process_inference_results(
    IModelRunner* runner,
    cv::Mat& rgb_mat,
    uint32_t& debug_frame_idx) noexcept
{
    if (!runner) return;

    auto* retinaface_runner = dynamic_cast<RKNNRetinafaceRunner*>(runner);
    auto* yolox_runner = dynamic_cast<RKNNYoloXRunner*>(runner);

    cv::Mat drawMat = rgb_mat; // alias for drawing

    if (retinaface_runner) {
        draw_face_detections(drawMat, retinaface_runner);
    } else if (yolox_runner) {
        draw_yolo_detections(drawMat, yolox_runner);
    }

    // Save annotated frame as JPEG
    save_debug_jpg(/*visFrame=*/rgb_mat,
                   /*path=*/"/tmp/output.jpg",
                   /*frame_idx=*/debug_frame_idx);
    debug_frame_idx++;
}

} // namespace visualization
