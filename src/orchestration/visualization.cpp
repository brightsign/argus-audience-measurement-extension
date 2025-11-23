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
    // V6.2.3.5.7: DISABLED - frame_writer handles output now (combined face + YOLOX)
    // Each worker has its own frame_writer, only YOLOX writer is enabled
    (void)visFrame;
    (void)path;
    (void)frame_idx;
}

static void draw_face_detections(
    cv::Mat& drawMat,
    const RKNNRetinafaceRunner* retinaface_runner,
    float scale_x,
    float scale_y, 
    int offset_x,
    int offset_y) noexcept
{
    const retinaface_result* result =
        static_cast<const retinaface_result*>(
            retinaface_runner->get_last_result());

    if (!result || result->count == 0) {
        static int no_face_log_count = 0;
        if (no_face_log_count < 10 || no_face_log_count % 30 == 0) {
            LG_INFO("[VIS-FACE] No face detections (result=%p, count=%d) log_count=%d",
                    result, result ? result->count : -1, no_face_log_count);
        }
        no_face_log_count++;
        return;
    }
    
    static int face_draw_count = 0;
    if (face_draw_count % 30 == 0) {
        LG_INFO("[VIS-FACE] Drawing %d face detections (count=%d)", 
                result->count, face_draw_count);
    }
    face_draw_count++;

    // V6.2.3.5.8: De-letterbox parameters (model/letterbox → camera coords)
    // RetinaFace outputs are in 320×320 model/letterbox space
    // Camera is 640×480, letterboxed to 320×240 inside 320×320 model
    const float s = 0.5f;       // min(320/640, 320/480)
    const float pad_x = 0.0f;   // (320 - 320)/2
    const float pad_y = 40.0f;  // (320 - 240)/2
    
    static int face_delbox_log = 0;
    if (face_delbox_log < 3) {
        LG_INFO("[RET] De-letterbox: cam=640x480 model=320x320 s=%.3f pad=(%.1f,%.1f)", 
                s, pad_x, pad_y);
        face_delbox_log++;
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

        // V6.2.3.5.8: TWO-STAGE TRANSFORM - model/letterbox → camera → canvas
        // Step 1: De-letterbox (model/letterbox → camera coords)
        const auto& box = obj.box;
        float cx0 = (box.left - pad_x) / s;
        float cy0 = (box.top - pad_y) / s;
        float cx1 = (box.right - pad_x) / s;
        float cy1 = (box.bottom - pad_y) / s;
        
        static int face_transform_log = 0;
        if (face_transform_log < 3) {
            LG_INFO("[RET] Face#%d model=(%.1f,%.1f,%.1f,%.1f) -> camera=(%.1f,%.1f,%.1f,%.1f)",
                    i, box.left, box.top, box.right, box.bottom, cx0, cy0, cx1, cy1);
            face_transform_log++;
        }
        
        // Step 2: Camera → canvas (scale by 0.5, then add offset_y=40)
        int x0 = (int)std::round(cx0 * scale_x) + offset_x;
        int y0 = (int)std::round(cy0 * scale_y) + offset_y;
        int x1 = (int)std::round(cx1 * scale_x) + offset_x;
        int y1 = (int)std::round(cy1 * scale_y) + offset_y;
        
        if (face_transform_log <= 3 && i == 0) {
            LG_INFO("[RET] Draw camera=(%.1f,%.1f,%.1f,%.1f) -> canvas=(%d,%d,%d,%d) scale=(%.3f,%.3f) off=(%d,%d)",
                    cx0, cy0, cx1, cy1, x0, y0, x1, y1, scale_x, scale_y, offset_x, offset_y);
        }
        
        cv::rectangle(
            drawMat,
            cv::Point(x0, y0),
            cv::Point(x1, y1),
            box_color,
            2
        );

        // V6.2.3.5.8: Transform landmarks (model/letterbox → camera → canvas)
        for (int lm = 0; lm < 5; ++lm) {
            // Step 1: De-letterbox (model/letterbox → camera)
            float cam_lx = (obj.ponit[lm].x - pad_x) / s;
            float cam_ly = (obj.ponit[lm].y - pad_y) / s;
            
            // Step 2: Camera → canvas (scale, then add offset_y=40)
            int lx = (int)std::round(cam_lx * scale_x) + offset_x;
            int ly = (int)std::round(cam_ly * scale_y) + offset_y;

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

        // put "attn" label if attending (use canvas coords)
        if (attending) {
            cv::putText(drawMat,
                        "ATTN",
                        cv::Point(x0, y0 - 4),
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
    RKNNYoloXRunner* yolox_runner,
    float scale_x = 1.0f,
    float scale_y = 1.0f,
    int offset_x = 0,
    int offset_y = 0) noexcept
{
    if (!yolox_runner) return;

    const Detection* dets = yolox_runner->get_detections();
    int det_count = yolox_runner->get_detection_count();

    if (!dets || det_count == 0) return;
    
    // V6.2.3.5: Debug what detections we received
    static int debug_vis_det_count = 0;
    if (debug_vis_det_count < 3) {
        LG_INFO("[VIS] Received %d detections from YOLOX", det_count);
        for (int i = 0; i < std::min(det_count, 3); ++i) {
            LG_INFO("[VIS]   Det#%d: class=%d score=%.2f bbox=(%.1f,%.1f,%.1f,%.1f)",
                    i, dets[i].class_id, dets[i].score,
                    dets[i].x0, dets[i].y0, dets[i].x1, dets[i].y1);
        }
        debug_vis_det_count++;
    }

    // Rainbow colors for visualization (BGR format)
    const cv::Scalar colors[] = {
        cv::Scalar(0, 0, 255),        // Red
        cv::Scalar(0, 165, 255),      // Orange
        cv::Scalar(0, 255, 255),      // Yellow
        cv::Scalar(0, 255, 0),        // Green
        cv::Scalar(255, 0, 0),        // Blue
        cv::Scalar(130, 0, 75),       // Indigo
        cv::Scalar(211, 0, 148),      // Violet
        cv::Scalar(255, 0, 127),      // Purple
        cv::Scalar(255, 255, 0),      // Cyan (repeat)
        cv::Scalar(0, 0, 0),          // Black
    };

    int person_count = 0;
    static int debug_draw_count = 0;
    for (int i = 0; i < det_count; ++i) {
        const auto& det = dets[i];

        // Only draw bounding boxes for person (class_id = 0)
        // Explicitly suppress other classes like tie (27), handbag (26), etc.
        if (det.class_id != 0) {
            if (debug_draw_count < 3 && i < 3) {
                LG_INFO("[VIS] Skipping Det#%d: class=%d (not person)", i, det.class_id);
            }
            continue;
        }

        person_count++;

        // Yellow color for person
        cv::Scalar box_color = cv::Scalar(0, 255, 255);  // Yellow in BGR

        // V6.2.3.5: Map camera coords to letterbox space (scale + offset for padding)
        // Detections are in original camera coords (e.g., 640x480)
        // Need to map to letterbox space on canvas (e.g., 640x480 letterboxed in 640x640)
        int x0 = (int)(det.x0 * scale_x) + offset_x;
        int y0 = (int)(det.y0 * scale_y) + offset_y;
        int x1 = (int)(det.x1 * scale_x) + offset_x;
        int y1 = (int)(det.y1 * scale_y) + offset_y;

        // V6.2.3.5.3: Enhanced debug - show what we're actually drawing
        if (debug_draw_count < 3 && i < 2) {
            LG_INFO("[VIS-DEBUG] Det#%d BEFORE transform: camera=(%.1f,%.1f,%.1f,%.1f)", 
                    i, det.x0, det.y0, det.x1, det.y1);
            LG_INFO("[VIS-DEBUG] Det#%d Transform params: scale=(%.3f,%.3f) offset=(%d,%d)",
                    i, scale_x, scale_y, offset_x, offset_y);
            LG_INFO("[VIS-DEBUG] Det#%d AFTER transform: canvas=(%d,%d,%d,%d) [will draw on CANVAS size=%dx%d]",
                    i, x0, y0, x1, y1, drawMat.cols, drawMat.rows);
            if (i == det_count - 1 || i == 1) debug_draw_count++;
        }

        cv::rectangle(
            drawMat,
            cv::Point(x0, y0),
            cv::Point(x1, y1),
            box_color,
            2
        );

        // Draw score label (simplified - just show confidence)
        char label[64];
        snprintf(label, sizeof(label), "person %.2f", det.score);

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
    LG_INFO("overlay: YOLO persons=%d (total_objects=%d)", person_count, det_count);
    #endif
}

void process_inference_results(
    IModelRunner* runner,
    cv::Mat& rgb_mat,
    uint32_t& debug_frame_idx,
    int orig_width,
    int orig_height,
    IModelRunner* second_runner) noexcept
{
    if (!runner) return;

    auto* retinaface_runner = dynamic_cast<RKNNRetinafaceRunner*>(runner);
    auto* yolox_runner = dynamic_cast<RKNNYoloXRunner*>(runner);
    
    // V6.2.3.5.7: Check second_runner for the other model type
    if (second_runner) {
        if (!retinaface_runner) {
            retinaface_runner = dynamic_cast<RKNNRetinafaceRunner*>(second_runner);
        }
        if (!yolox_runner) {
            yolox_runner = dynamic_cast<RKNNYoloXRunner*>(second_runner);
        }
    }

    cv::Mat drawMat = rgb_mat; // alias for drawing
    
    // V6.2.3.5.4: Log actual canvas dimensions at draw time
    LG_INFO("[VIS-CANVAS] drawMat actual size: %dx%d (rgb_mat: %dx%d)", 
            drawMat.cols, drawMat.rows, rgb_mat.cols, rgb_mat.rows);
    
    // V6.2.3.5: FIXED - Calculate letterbox parameters to map camera coords to canvas
    // The visualization canvas (rgb_mat) shows the LETTERBOXED image (e.g., 640x640)
    // Detections are in camera coordinate space (e.g., 640x480)
    // We need to map camera coords back to letterbox space (reverse of de-letterbox)
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    int offset_x = 0;
    int offset_y = 0;
    
    if (orig_width > 0 && orig_height > 0 && rgb_mat.cols > 0 && rgb_mat.rows > 0) {
        // Calculate letterbox parameters (same as in resize_frame_rga)
        const float letterbox_scale = std::min((float)rgb_mat.cols / orig_width, 
                                              (float)rgb_mat.rows / orig_height);
        const int letterbox_w = (int)(orig_width * letterbox_scale);
        const int letterbox_h = (int)(orig_height * letterbox_scale);
        offset_x = (rgb_mat.cols - letterbox_w) / 2;
        offset_y = (rgb_mat.rows - letterbox_h) / 2;
        
        // Scale factor to map camera coords to letterbox space
        scale_x = letterbox_scale;
        scale_y = letterbox_scale;
        
        // V6.2.3.5.3: Enhanced debug logging  
        static int debug_vis_count = 0;
        if (debug_vis_count < 3) {
            LG_INFO("[VIS-INIT] canvas_size=%dx%d camera_size=%dx%d",
                    rgb_mat.cols, rgb_mat.rows, orig_width, orig_height);
            LG_INFO("[VIS-INIT] letterbox_scale=%.3f letterbox_size=%dx%d offset=(%d,%d)",
                    letterbox_scale, letterbox_w, letterbox_h, offset_x, offset_y);
            LG_INFO("[VIS-INIT] transform: scale_x=%.3f scale_y=%.3f offset_x=%d offset_y=%d",
                    scale_x, scale_y, offset_x, offset_y);
            debug_vis_count++;
        }
    }

    // V6.2.3.5.6: Draw both face AND YOLOX detections (not mutually exclusive)
    // Both need coordinate transforms from camera space to canvas space
    static int draw_debug_count = 0;
    if (draw_debug_count < 3) {
        LG_INFO("[VIS-DRAW] retinaface_runner=%p yolox_runner=%p", 
                retinaface_runner, yolox_runner);
        draw_debug_count++;
    }
    
    if (retinaface_runner) {
        draw_face_detections(drawMat, retinaface_runner, scale_x, scale_y, offset_x, offset_y);
    }
    if (yolox_runner) {
        draw_yolo_detections(drawMat, yolox_runner, scale_x, scale_y, offset_x, offset_y);
    }

    // V6.2.3.5.4: Draw guide rectangle to validate letterbox boundaries
    // For 320x320 canvas showing 640x480 camera at 0.5 scale:
    // - Camera letterboxed region: 320x240 (0.5 * 640 x 0.5 * 480)
    // - Y-offset: 40px to center 240px height in 320px canvas
    // This blue rectangle should contain the entire visible scene
    static int guide_debug_count = 0;
    if (guide_debug_count < 5) {
        LG_INFO("[VIS-GUIDE-CHECK] canvas=%dx%d camera=%dx%d condition=%s",
                rgb_mat.cols, rgb_mat.rows, orig_width, orig_height,
                (rgb_mat.cols == 320 && rgb_mat.rows == 320 && orig_width == 640 && orig_height == 480) ? "YES" : "NO");
        guide_debug_count++;
    }
    
    if (rgb_mat.cols == 320 && rgb_mat.rows == 320 && orig_width == 640 && orig_height == 480) {
        cv::rectangle(drawMat, 
                      cv::Point(0, 40),           // Top-left at y-offset
                      cv::Point(320, 280),        // Bottom-right (320x240 rect)
                      cv::Scalar(255, 0, 0),      // Blue color (BGR)
                      2);                          // 2px line
        LG_INFO("[VIS-GUIDE] Drew 320x240 blue guide rectangle at offset (0,40) to (320,280)");
    }

    // Save annotated frame as JPEG
    save_debug_jpg(/*visFrame=*/rgb_mat,
                   /*path=*/"/tmp/output.jpg",
                   /*frame_idx=*/debug_frame_idx);
    debug_frame_idx++;
}

} // namespace visualization
