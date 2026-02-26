#include "orchestration/visualization.h"
#include "orchestration/inference_worker.h"  // For FusionResults
#include "output/face_blur.h"
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

// V7.0.2: Updated to use FusionResults for synchronized face detection data
// V7.1: Added orig_width/orig_height for dynamic de-letterbox calculation
static void draw_face_detections(
    cv::Mat& drawMat,
    const RKNNRetinafaceRunner* retinaface_runner,
    float scale_x,
    float scale_y, 
    int offset_x,
    int offset_y,
    FusionResults* fusion_output,
    int orig_width,
    int orig_height) noexcept
{
    // V7.0.2: Use FusionResults directly for synchronized detection data
    if (!fusion_output) {
        // Fallback: use runner cache if no fusion output available
        const retinaface_result* result = static_cast<const retinaface_result*>(
                retinaface_runner->get_last_result());
        if (!result || result->count == 0) {
            return;
        }
        // Draw from cached result (legacy path)
        for (int i = 0; i < result->count; i++) {
            const auto& obj = result->object[i];
            int x0 = static_cast<int>(obj.box.left * scale_x) + offset_x;
            int y0 = static_cast<int>(obj.box.top * scale_y) + offset_y;
            int x1 = static_cast<int>(obj.box.right * scale_x) + offset_x;
            int y1 = static_cast<int>(obj.box.bottom * scale_y) + offset_y;
            cv::rectangle(drawMat, cv::Point(x0, y0), cv::Point(x1, y1), 
                         cv::Scalar(0, 255, 0), 2);
        }
        return;
    }
    
    // Lock fusion output and copy detections + landmarks for processing
    std::vector<Detection> face_dets_copy;
    std::vector<Landmarks> face_lms_copy;
    uint64_t face_seq_copy = 0;
    {
        std::lock_guard<std::mutex> g(fusion_output->m);
        if (fusion_output->face_dets.empty() || fusion_output->face_seq == 0) {
            static int no_face_log_count = 0;
            if (no_face_log_count < 10 || no_face_log_count % 100 == 0) {
                LG_INFO("[VIS-FACE] No face detections in fusion (seq=%llu, dets=%zu)",
                        (unsigned long long)fusion_output->face_seq,
                        fusion_output->face_dets.size());
            }
            no_face_log_count++;
            return;
        }
        face_dets_copy = fusion_output->face_dets;
        face_lms_copy = fusion_output->face_lms;
        face_seq_copy = fusion_output->face_seq;
    }
    
    // Log periodically
    static int face_draw_count = 0;
    if (face_draw_count < 10 || face_draw_count % 30 == 0) {
        LG_INFO("[VIS-FACE] Drawing %zu face detections from fusion (seq=%llu, count=%d)", 
                face_dets_copy.size(), (unsigned long long)face_seq_copy, face_draw_count);
    }
    face_draw_count++;

    // V7.1: DYNAMIC de-letterbox calculation based on actual camera dimensions
    // RetinaFace outputs are in 320×320 model/letterbox space
    // Camera could be any resolution (e.g., 640×480, 1920×1080)
    // Calculate letterbox parameters for this camera resolution
    const float model_size = 320.0f;
    const float s = std::min(model_size / orig_width, model_size / orig_height);  // letterbox scale
    const float letterbox_w = orig_width * s;
    const float letterbox_h = orig_height * s;
    const float pad_x = (model_size - letterbox_w) / 2.0f;
    const float pad_y = (model_size - letterbox_h) / 2.0f;
    
    static int face_delbox_log = 0;
    if (face_delbox_log < 3) {
        LG_INFO("[RET] De-letterbox: cam=%dx%d model=320x320 s=%.3f pad=(%.1f,%.1f)", 
                orig_width, orig_height, s, pad_x, pad_y);
        face_delbox_log++;
    }

    // Draw face detections from fusion data
    for (size_t i = 0; i < face_dets_copy.size(); ++i) {
        const auto& det = face_dets_copy[i];
        
        // V7.0.2: Compute attention (gaze direction) from landmarks
        bool attending = false;
        if (i < face_lms_copy.size()) {
            const auto& lm = face_lms_copy[i];
            
            // Extract eye landmarks (pts[0,1] = left_eye, pts[2,3] = right_eye)
            float left_eye_x = lm.pts[0];
            float left_eye_y = lm.pts[1];
            float right_eye_x = lm.pts[2];
            float right_eye_y = lm.pts[3];
            
            // Compute interocular distance
            float dx = left_eye_x - right_eye_x;
            float dy = left_eye_y - right_eye_y;
            float interocular_dist_pix = std::sqrt(dx * dx + dy * dy);
            
            // Compute face dimensions
            float face_width = det.x1 - det.x0;
            float face_height = det.y1 - det.y0;
            float face_aspect_ratio = face_height / face_width;
            float interocular_face_ratio = interocular_dist_pix / face_width;
            
            // Attention detection heuristic (same as face_is_looking_at_us)
            // Face aspect ratio should be ~1.618 (golden ratio)
            // Interocular ratio should be ~0.5
            // Relaxed thresholds: 1.0-2.5 for aspect, 0.25-0.75 for interocular
            attending = (face_aspect_ratio > 1.0 && face_aspect_ratio < 2.5 &&
                        interocular_face_ratio > 0.25 && interocular_face_ratio < 0.75);
            
            // DEBUG: Log when green box is drawn for timing analysis
            if (attending) {
              static int green_box_counter = 0;
              if (++green_box_counter % 5 == 0) {  // Every 5th green box (~0.5s at 10 Hz)
                LG_INFO("[VIZ-GAZE] Drawing GREEN box for face #%zu (aspect=%.2f, ioc=%.2f)", 
                        i, face_aspect_ratio, interocular_face_ratio);
              }
            }
        }
        
        // Choose box color: green if looking, red otherwise
        cv::Scalar box_color = attending
            ? cv::Scalar(0, 255, 0)      // green (BGR)
            : cv::Scalar(0, 0, 255);     // red (BGR)

        // V7.0.2: Draw face boxes with attention-based color
        // Detection coords are already in model space (x0, y0, x1, y1)
        
        // V6.2.3.5.8: TWO-STAGE TRANSFORM - model/letterbox → camera → canvas
        // Step 1: De-letterbox (model/letterbox → camera coords)
        float cx0 = (det.x0 - pad_x) / s;
        float cy0 = (det.y0 - pad_y) / s;
        float cx1 = (det.x1 - pad_x) / s;
        float cy1 = (det.y1 - pad_y) / s;
        
        static int face_transform_log = 0;
        if (face_transform_log < 3 && i == 0) {
            LG_INFO("[RET] Face#%zu model=(%.1f,%.1f,%.1f,%.1f) -> camera=(%.1f,%.1f,%.1f,%.1f)",
                    i, det.x0, det.y0, det.x1, det.y1, cx0, cy0, cx1, cy1);
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
            box_color,  // Green if attending, red otherwise
            2
        );
        
        // Add "ATTN" label if attending
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
        
        // V7.0.2: Draw facial landmarks (eyes, nose, mouth) if available
        // Landmarks structure: float pts[10] = {x0,y0, x1,y1, x2,y2, x3,y3, x4,y4}
        // 5 points: left_eye, right_eye, nose, left_mouth, right_mouth
        if (i < face_lms_copy.size()) {
            const auto& lm = face_lms_copy[i];
            
            // Draw 5 facial landmarks
            for (int lm_idx = 0; lm_idx < 5; ++lm_idx) {
                float model_x = lm.pts[lm_idx * 2];
                float model_y = lm.pts[lm_idx * 2 + 1];
                
                // V6.2.3.5.8: Transform landmarks (model/letterbox → camera → canvas)
                // Step 1: De-letterbox (model/letterbox → camera)
                float cam_lx = (model_x - pad_x) / s;
                float cam_ly = (model_y - pad_y) / s;
                
                // Step 2: Camera → canvas (scale, then add offset_y=40)
                int lx = (int)std::round(cam_lx * scale_x) + offset_x;
                int ly = (int)std::round(cam_ly * scale_y) + offset_y;

                // Eye landmarks cyan, others yellow
                cv::Scalar lm_color = (lm_idx < 2)
                    ? cv::Scalar(255, 255, 0)    // cyan (BGR)
                    : cv::Scalar(0, 255, 255);   // yellow (BGR)

                cv::circle(
                    drawMat,
                    cv::Point(lx, ly),
                    2,
                    lm_color,
                    2,
                    cv::LINE_AA
                );
            }
        }
    }
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
    const float min_confidence = 0.25f;  // Lower threshold to catch distant people

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

        // Skip low-confidence detections (shadows, false positives)
        if (det.score < min_confidence) {
            if (debug_draw_count < 3) {
                LG_INFO("[VIS] Skipping Det#%d: score=%.2f below threshold %.2f", i, det.score, min_confidence);
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

// Apply blur to person bounding boxes BEFORE drawing overlays
// Blur area is inset by box_thickness so drawn bounding boxes remain unblurred
static void blur_persons(
    cv::Mat& drawMat,
    FusionResults* fusion_output,
    const output::BlurConfig& blur_config,
    int orig_width,
    int orig_height,
    float scale_x,
    float scale_y,
    int offset_x,
    int offset_y,
    int box_thickness = 2) noexcept
{
    if (!blur_config.enabled || !fusion_output) return;

    // Lock fusion output and get YOLOX person detections
    std::vector<Detection> yolo_dets_copy;
    {
        std::lock_guard<std::mutex> g(fusion_output->m);
        if (fusion_output->yolo_dets.empty()) return;
        yolo_dets_copy = fusion_output->yolo_dets;
    }

    // De-letterbox parameters for YOLOX (640x640 model)
    const float yolo_model_size = 640.0f;
    const float ys = std::min(yolo_model_size / orig_width, yolo_model_size / orig_height);
    const float ypad_x = (yolo_model_size - orig_width * ys) / 2.0f;
    const float ypad_y = (yolo_model_size - orig_height * ys) / 2.0f;

    std::vector<cv::Rect> person_bboxes;
    person_bboxes.reserve(yolo_dets_copy.size());

    const float min_confidence = 0.25f;  // Lower threshold to catch distant people

    for (const auto& det : yolo_dets_copy) {
        // Only blur person class (class_id == 0 in COCO)
        if (det.class_id != 0) continue;
        if (det.score < min_confidence) continue;

        // De-letterbox: YOLOX model space (640x640) -> camera space
        float cx0 = (det.x0 - ypad_x) / ys;
        float cy0 = (det.y0 - ypad_y) / ys;
        float cx1 = (det.x1 - ypad_x) / ys;
        float cy1 = (det.y1 - ypad_y) / ys;

        // Camera space -> canvas space
        int x0 = (int)(cx0 * scale_x) + offset_x;
        int y0 = (int)(cy0 * scale_y) + offset_y;
        int x1 = (int)(cx1 * scale_x) + offset_x;
        int y1 = (int)(cy1 * scale_y) + offset_y;

        // Inset by box_thickness so bounding box lines remain unblurred
        x0 += box_thickness;
        y0 += box_thickness;
        x1 -= box_thickness;
        y1 -= box_thickness;

        // Only add valid rects
        if (x1 > x0 && y1 > y0) {
            person_bboxes.push_back(cv::Rect(x0, y0, x1 - x0, y1 - y0));
        }
    }

    if (!person_bboxes.empty()) {
        output::blur_faces(drawMat, person_bboxes, blur_config);
        static int blur_log_count = 0;
        if (blur_log_count < 5 || blur_log_count % 30 == 0) {
            LG_INFO("[VIS-BLUR] Applied person blur to %zu regions (inset=%dpx)",
                    person_bboxes.size(), box_thickness);
        }
        blur_log_count++;
    }
}

// V7.0.2: Updated to accept FusionResults for synchronized detection access
// V7.2: Added blur_config for applying person blur before drawing boxes
void process_inference_results(
    IModelRunner* runner,
    cv::Mat& rgb_mat,
    uint32_t& debug_frame_idx,
    int orig_width,
    int orig_height,
    IModelRunner* second_runner,
    FusionResults* fusion_output,
    const output::BlurConfig& blur_config) noexcept
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

    // V7.2: Apply person blur BEFORE drawing bounding boxes
    // Blur is applied inside the box area so box lines remain unblurred
    if (blur_config.enabled && orig_width > 0 && orig_height > 0) {
        blur_persons(drawMat, fusion_output, blur_config,
                    orig_width, orig_height,
                    scale_x, scale_y, offset_x, offset_y,
                    2);  // box_thickness = 2px inset
    }

    // V6.2.3.5.6: Draw both face AND YOLOX detections (not mutually exclusive)
    // Both need coordinate transforms from camera space to canvas space
    // V7.0.2: Pass fusion_output for synchronized face detection access
    // V7.1: Pass orig_width/orig_height for dynamic de-letterbox calculation
    static int draw_debug_count = 0;
    if (draw_debug_count < 3) {
        LG_INFO("[VIS-DRAW] retinaface_runner=%p yolox_runner=%p fusion_output=%p blur=%s",
                retinaface_runner, yolox_runner, fusion_output,
                blur_config.enabled ? "enabled" : "disabled");
        draw_debug_count++;
    }

    if (retinaface_runner) {
        draw_face_detections(drawMat, retinaface_runner, scale_x, scale_y, offset_x, offset_y, fusion_output, orig_width, orig_height);
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
