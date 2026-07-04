#include "orchestration/inference_worker.h"
#include "orchestration/visualization.h"
#include "models/model_runner.h"
#include "models/model_runner_retinaface.h"
#include "models/model_runner_yolox.h"
#include "pipeline/shared_frame.h"
#include "pipeline/frame_mailbox.h"
#include "pipeline/pipeline_types.h"
#include "config/model_spec.h"
#include "image_utils.h"
#include "common.h"
#include "metrics/log_global.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <rga/rga.h>
#include <rga/im2d.h>
#include <thread>
#include <chrono>
#include <cstring>

namespace inference_worker {

// Local helper: Check if we should process this frame based on skip count
static bool should_process_frame(uint64_t seq, int skip_frames) noexcept {
    if (skip_frames <= 1) return true;                  // Process every frame
    return (seq % static_cast<uint64_t>(skip_frames)) == 0;  // Process every Nth frame
}

// Local helper: Resize frame via RGA hardware acceleration (with OpenCV fallback)
// Input is BGR24 (from USB/RTSP, already in model color space)
// Output is BGR24 (ready for model inference)
static bool resize_frame_rga(
    const std::vector<uint8_t>& src_bgr,
    int src_w,
    int src_h,
    std::vector<uint8_t>& dst_buf,
    int dst_w,
    int dst_h) noexcept
{
    // If already the right size, just copy
    if (src_w == dst_w && src_h == dst_h) {
        size_t bytes = size_t(dst_w) * dst_h * 3;
        if (dst_buf.size() != bytes) dst_buf.resize(bytes);
        std::memcpy(dst_buf.data(), src_bgr.data(), bytes);
        return true;
    }

    // V6.2.3.4: CRITICAL FIX - Do letterbox resize, not stretch!
    // YOLOX expects letterboxed input (maintain aspect ratio + black padding)
    // Previous code was stretching/squashing, causing wrong detections

    // Calculate letterbox parameters
    float scale = std::min(float(dst_w) / src_w, float(dst_h) / src_h);
    int letterbox_w = int(src_w * scale);
    int letterbox_h = int(src_h * scale);
    int pad_x = (dst_w - letterbox_w) / 2;
    int pad_y = (dst_h - letterbox_h) / 2;

    // Resize destination buffer to target size and fill with black
    if (dst_buf.size() != size_t(dst_w) * dst_h * 3) {
        dst_buf.resize(size_t(dst_w) * dst_h * 3);
    }
    std::fill(dst_buf.begin(), dst_buf.end(), 0);  // Black padding

    // Fast path: offload the aspect-ratio resize to the RGA hardware block.
    // improcess() scales the whole source into the destination ROI (drect),
    // which gives us the letterbox placement in one hardware call. The black
    // padding is already in place from the fill above.
    rga_buffer_t src_rga = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(src_bgr.data()), src_w, src_h, RK_FORMAT_BGR_888);
    rga_buffer_t dst_rga = wrapbuffer_virtualaddr(
        dst_buf.data(), dst_w, dst_h, RK_FORMAT_BGR_888);

    im_rect srect{0, 0, src_w, src_h};
    im_rect drect{pad_x, pad_y, letterbox_w, letterbox_h};
    im_rect prect{};
    rga_buffer_t pat{};

    IM_STATUS st = improcess(src_rga, dst_rga, pat, srect, drect, prect, 0);
    if (st == IM_STATUS_SUCCESS) {
        return true;
    }

    // Fallback: OpenCV letterbox resize on the CPU (SIMD).
    cv::Mat src_mat(src_h, src_w, CV_8UC3, const_cast<uint8_t*>(src_bgr.data()));
    cv::Mat dst_full(dst_h, dst_w, CV_8UC3, dst_buf.data());

    // Create ROI in destination for the letterboxed image (skip padding area)
    cv::Mat dst_roi = dst_full(cv::Rect(pad_x, pad_y, letterbox_w, letterbox_h));

    // Resize source to fit in ROI (maintains aspect ratio)
    cv::resize(src_mat, dst_roi, cv::Size(letterbox_w, letterbox_h), 0, 0, cv::INTER_LINEAR);

    return true;
}

// Local helper: Prepare FrameView from resized BGR buffer
static void prepare_frame_view(
    FrameView& fv_in,
    const std::vector<uint8_t>& bgr_buf,
    int dst_w,
    int dst_h,
    const std::shared_ptr<SharedFrame>& sf) noexcept
{
    fv_in.fmt     = PixelFormat::BGR24;  // BGR24 input (not RGB24)
    fv_in.width   = dst_w;
    fv_in.height  = dst_h;
    fv_in.stride0 = dst_w * 3;
    fv_in.plane0  = const_cast<uint8_t*>(bgr_buf.data());
    fv_in.pts_ns  = sf->pts_ns;
    // V7.1: FIXED - Store ACTUAL original camera dimensions (not preprocessed dims)
    // RTSP: sf->orig_width=1920, sf->orig_height=1080 (NOT sf->width=320)
    // This allows YOLOX to properly de-letterbox detections back to camera space
    fv_in.orig_width  = sf->orig_width;
    fv_in.orig_height = sf->orig_height;
}

// Local helper: Store inference results in fusion output
static void store_inference_results(
    IModelRunner* runner,
    const InferenceOutputs& outs,
    FusionResults* fusion_output,
    uint64_t seq) noexcept 
{
    if (!fusion_output) return;

    std::lock_guard<std::mutex> g(fusion_output->m);

    auto* retinaface_runner = dynamic_cast<RKNNRetinafaceRunner*>(runner);
    auto* yolox_runner = dynamic_cast<RKNNYoloXRunner*>(runner);

    if (retinaface_runner) {
        fusion_output->face_dets.assign(outs.dets, outs.dets + outs.num_dets);
        fusion_output->face_lms.assign(outs.lms, outs.lms + outs.num_lms);
        fusion_output->face_seq = seq;
        
        // Debug: Log face detection counts periodically
        static int face_log_counter = 0;
        if (++face_log_counter % 90 == 0) {  // Every 3 seconds at 30 FPS
            LG_INFO("[RETINAFACE] Detected %d faces (seq=%llu)", 
                    outs.num_dets, (unsigned long long)seq);
            for (int i = 0; i < outs.num_dets && i < 5; ++i) {  // Log first 5 faces
                const auto& det = outs.dets[i];
                LG_INFO("[RETINAFACE] Face %d: bbox=[%.1f,%.1f,%.1f,%.1f] score=%.2f", 
                        i, det.x0, det.y0, det.x1, det.y1, det.score);
            }
        }
        
        #ifdef ENABLE_DEBUG
        if (seq % 30 == 0) {
            LG_INFO("store_inference_results: RetinaFace seq=%llu dets=%d (yolo_seq=%llu)",
                    (unsigned long long)seq, outs.num_dets, 
                    (unsigned long long)fusion_output->yolo_seq);
        }
        #endif
    } else if (yolox_runner) {
        fusion_output->yolo_dets.assign(outs.dets, outs.dets + outs.num_dets);
        fusion_output->yolo_seq = seq;
        
        #ifdef ENABLE_DEBUG
        if (seq % 30 == 0) {
            LG_INFO("store_inference_results: YOLOX seq=%llu dets=%d (face_seq=%llu)",
                    (unsigned long long)seq, outs.num_dets, 
                    (unsigned long long)fusion_output->face_seq);
        }
        #endif
    }
}

void run_inference_loop(
    IModelRunner* runner,
    std::shared_ptr<FrameMailbox> frame_mailbox,
    FusionResults* fusion_output,
    IFrameWriter* frame_writer,
    const WorkerConfig& config,
    const std::atomic<bool>& stop_flag,
    IModelRunner* second_runner) noexcept
{
    try {
        LG_INFO("inference_worker: start (%s, skip_frames=%d%s)",
                config.model_name.c_str(), config.skip_frames,
                second_runner ? " with second_runner" : "");

        if (!runner) {
            LG_ERROR("inference_worker: runner is null");
            return;
        }

        const int dst_w = config.model_input_width;
        const int dst_h = config.model_input_height;

        // Preallocate resized buffer (reused every frame)
        std::vector<uint8_t> rgb_resized_buf(dst_w * dst_h * 3);

        uint64_t last_logged_seq = 0;
        bool logged_first_frame = false;
        uint32_t debug_frame_idx = 0;
        
        // FPS tracking
        auto fps_start_time = std::chrono::steady_clock::now();
        int fps_frame_count = 0;
        int fps_processed_count = 0;

        // Method 2: per-stage timing accumulators (nanoseconds), reset each second
        const bool perf = config.log_performance;
        int64_t acc_resize_ns = 0;
        int64_t acc_infer_ns  = 0;   // wall time around runner->infer()
        int64_t acc_pre_ns    = 0;   // model-reported preprocess
        int64_t acc_npu_ns    = 0;   // model-reported NPU inference
        int64_t acc_post_ns   = 0;   // model-reported postprocess
        int64_t acc_vis_ns    = 0;   // clone + draw overlays
        int64_t acc_write_ns  = 0;   // frame writer
        int      perf_samples = 0;
        auto perf_start_time = std::chrono::steady_clock::now();

        while (!stop_flag.load(std::memory_order_relaxed)) {
            // Get next frame from mailbox
            auto sf = frame_mailbox->takeFrame();
            if (!sf) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            // Track all frames (for input FPS)
            fps_frame_count++;

            // Skip frames based on configuration
            if (!should_process_frame(sf->seq, config.skip_frames)) {
                #ifdef ENABLE_DEBUG
                if (sf->seq % 100 == 0) {
                    LG_INFO("inference_worker: skipped frame seq=%llu (%s)",
                            (unsigned long long)sf->seq, config.model_name.c_str());
                }
                #endif
                continue;
            }

            // Track processed frames (for inference FPS)
            fps_processed_count++;
            
            #ifdef ENABLE_DEBUG
            if (sf->seq % 30 == 0) {
                LG_INFO("inference_worker: processing frame seq=%llu (%s)",
                        (unsigned long long)sf->seq, config.model_name.c_str());
            }
            #endif

            // Log first frame reception
            if (!logged_first_frame) {
                LG_INFO("inference_worker: got first frame seq=%llu %dx%d (%s)",
                        (unsigned long long)sf->seq, sf->width, sf->height,
                        config.model_name.c_str());
                logged_first_frame = true;
            }

            // Validate frame data
            if (sf->bgr.empty()) {
                LG_WARN("inference_worker: frame has empty BGR data (seq=%llu, %s)",
                        (unsigned long long)sf->seq, config.model_name.c_str());
                continue;
            }

            // Resize frame via RGA hardware accelerator (BGR24 → BGR24)
            auto t_resize0 = perf ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
            if (!resize_frame_rga(sf->bgr, sf->width, sf->height,
                                  rgb_resized_buf, dst_w, dst_h)) {
                LG_WARN("inference_worker: RGA resize failed (seq=%llu, %s)",
                        (unsigned long long)sf->seq, config.model_name.c_str());
                continue;
            }
            if (perf) {
                acc_resize_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t_resize0).count();
            }

            // Prepare input frame view
            FrameView fv_in{};
            prepare_frame_view(fv_in, rgb_resized_buf, dst_w, dst_h, sf);

            // Run inference
            InferenceOutputs outs{};
            auto t_infer0 = perf ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
            if (!runner->infer(fv_in, outs)) {
                LG_WARN("inference_worker: inference failed (seq=%llu, %s)",
                        (unsigned long long)sf->seq, config.model_name.c_str());
                continue;
            }
            if (perf) {
                acc_infer_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t_infer0).count();
                acc_pre_ns  += runner->last_pre_ns();
                acc_npu_ns  += runner->last_infer_ns();
                acc_post_ns += runner->last_post_ns();
            }

            // Store results in fusion output FIRST (before visualization)
            store_inference_results(runner, outs, fusion_output, sf->seq);

            // Draw overlays on full-resolution camera frame (sf->bgr is now at original RTSP res)
            // Detection coords are in camera/orig space; with canvas == camera size, scale=1.0, offset=0
            auto t_vis0 = perf ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
            cv::Mat vis_canvas = cv::Mat(sf->height, sf->width, CV_8UC3,
                                        const_cast<uint8_t*>(sf->bgr.data())).clone();

            // Flip the raw background first so the image pixels are correct, then draw overlays
            // using mirrored coordinates so that text/box labels remain readable (not mirrored).
            if (config.flip_horizontal) {
                cv::flip(vis_canvas, vis_canvas, 1);  // 1 = horizontal flip
            }

            visualization::process_inference_results(runner, vis_canvas, debug_frame_idx,
                                                     sf->orig_width, sf->orig_height, second_runner,
                                                     fusion_output, config.blur_config,
                                                     config.flip_horizontal);

            // (flip already applied above — no post-visualization flip needed)
            if (perf) {
                acc_vis_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t_vis0).count();
            }

            // Write frame to disk if writer is available
            auto t_write0 = perf ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
            if (frame_writer) {
                PipelineResult result{};
                result.seq = sf->seq;
                result.frame_width = sf->orig_width;    // Original camera dimensions for letterbox crop
                result.frame_height = sf->orig_height;

                // Populate face tracks for blur processing
                // Transform coordinates: RetinaFace model space (320x320) -> canvas space (dst_w x dst_h)
                if (fusion_output && sf->orig_width > 0 && sf->orig_height > 0) {
                    std::lock_guard<std::mutex> g(fusion_output->m);
                    if (!fusion_output->face_dets.empty()) {
                        // Step 1: De-letterbox parameters (RetinaFace model is 320x320)
                        const float model_size = 320.0f;
                        const float s = std::min(model_size / sf->orig_width, model_size / sf->orig_height);
                        const float pad_x = (model_size - sf->orig_width * s) / 2.0f;
                        const float pad_y = (model_size - sf->orig_height * s) / 2.0f;

                        // Step 2: Canvas letterbox parameters
                        const float canvas_scale = std::min((float)dst_w / sf->orig_width,
                                                           (float)dst_h / sf->orig_height);
                        const int offset_x = (dst_w - (int)(sf->orig_width * canvas_scale)) / 2;
                        const int offset_y = (dst_h - (int)(sf->orig_height * canvas_scale)) / 2;

                        result.tracks.reserve(fusion_output->face_dets.size());
                        for (size_t i = 0; i < fusion_output->face_dets.size(); ++i) {
                            const auto& det = fusion_output->face_dets[i];

                            // De-letterbox: model space -> camera space
                            float cx0 = (det.x0 - pad_x) / s;
                            float cy0 = (det.y0 - pad_y) / s;
                            float cx1 = (det.x1 - pad_x) / s;
                            float cy1 = (det.y1 - pad_y) / s;

                            // Scale: camera space -> canvas space
                            Track t;
                            t.box.x0 = cx0 * canvas_scale + offset_x;
                            t.box.y0 = cy0 * canvas_scale + offset_y;
                            t.box.x1 = cx1 * canvas_scale + offset_x;
                            t.box.y1 = cy1 * canvas_scale + offset_y;
                            t.box.score = det.score;
                            t.box.class_id = det.class_id;

                            if (i < fusion_output->face_lms.size()) {
                                t.lms = fusion_output->face_lms[i];
                            }
                            result.tracks.push_back(t);
                        }
                    }

                    // Populate person tracks from YOLOX detections for person blur
                    // Transform coordinates: YOLOX model space (640x640) -> canvas space (dst_w x dst_h)
                    if (!fusion_output->yolo_dets.empty()) {
                        // Step 1: De-letterbox parameters (YOLOX model is 640x640)
                        const float yolo_model_size = 640.0f;
                        const float ys = std::min(yolo_model_size / sf->orig_width, yolo_model_size / sf->orig_height);
                        const float ypad_x = (yolo_model_size - sf->orig_width * ys) / 2.0f;
                        const float ypad_y = (yolo_model_size - sf->orig_height * ys) / 2.0f;

                        // Step 2: Canvas letterbox parameters (same as face)
                        const float ycanvas_scale = std::min((float)dst_w / sf->orig_width,
                                                             (float)dst_h / sf->orig_height);
                        const int yoffset_x = (dst_w - (int)(sf->orig_width * ycanvas_scale)) / 2;
                        const int yoffset_y = (dst_h - (int)(sf->orig_height * ycanvas_scale)) / 2;

                        result.person_tracks.reserve(fusion_output->yolo_dets.size());
                        for (const auto& det : fusion_output->yolo_dets) {
                            // Filter: only person class (class_id == 0 in COCO)
                            if (det.class_id != 0) continue;
                            if (det.score < 0.25f) continue;  // Lower threshold to catch distant people

                            // De-letterbox: model space -> camera space
                            float cx0 = (det.x0 - ypad_x) / ys;
                            float cy0 = (det.y0 - ypad_y) / ys;
                            float cx1 = (det.x1 - ypad_x) / ys;
                            float cy1 = (det.y1 - ypad_y) / ys;

                            // Scale: camera space -> canvas space
                            TrackedBox tb{};
                            tb.id = -1;  // No tracking ID in this context
                            tb.x0 = cx0 * ycanvas_scale + yoffset_x;
                            tb.y0 = cy0 * ycanvas_scale + yoffset_y;
                            tb.x1 = cx1 * ycanvas_scale + yoffset_x;
                            tb.y1 = cy1 * ycanvas_scale + yoffset_y;
                            tb.score = det.score;
                            result.person_tracks.push_back(tb);
                        }
                    }
                }

                frame_writer->writeFrame(vis_canvas, result);
            }
            if (perf) {
                acc_write_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t_write0).count();
                perf_samples++;
            }

            // Method 2: emit per-stage timing breakdown every second when enabled.
            if (perf) {
                auto perf_now = std::chrono::steady_clock::now();
                auto perf_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    perf_now - perf_start_time).count();
                if (perf_elapsed_ms >= 1000 && perf_samples > 0) {
                    const double inv = 1.0 / (perf_samples * 1e6);  // ns total -> ms avg
                    const double per_frame_fps =
                        (perf_elapsed_ms > 0) ? (1000.0 * perf_samples / perf_elapsed_ms) : 0.0;
                    LG_INFO("inference_worker: PERF (%s) fps=%.1f frames=%d | "
                            "resize=%.2fms infer=%.2fms (pre=%.2f npu=%.2f post=%.2f) "
                            "vis=%.2fms write=%.2fms total=%.2fms",
                            config.model_name.c_str(), per_frame_fps, perf_samples,
                            acc_resize_ns * inv,
                            acc_infer_ns  * inv,
                            acc_pre_ns    * inv,
                            acc_npu_ns    * inv,
                            acc_post_ns   * inv,
                            acc_vis_ns    * inv,
                            acc_write_ns  * inv,
                            (acc_resize_ns + acc_infer_ns + acc_vis_ns + acc_write_ns) * inv);
                    acc_resize_ns = acc_infer_ns = acc_pre_ns = acc_npu_ns = 0;
                    acc_post_ns = acc_vis_ns = acc_write_ns = 0;
                    perf_samples = 0;
                    perf_start_time = perf_now;
                }
            }

            // Log periodically
            #ifdef ENABLE_DEBUG
            // Log FPS metrics every second
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_start_time).count();
            
            if (elapsed_ms >= 1000) {
                float input_fps = (elapsed_ms > 0) ? (1000.0f * fps_frame_count / elapsed_ms) : 0.0f;
                float process_fps = (elapsed_ms > 0) ? (1000.0f * fps_processed_count / elapsed_ms) : 0.0f;
                LG_INFO("inference_worker: FPS metrics (%s) input=%.1f process=%.1f skip=%d frames=%d",
                        config.model_name.c_str(), input_fps, process_fps, 
                        config.skip_frames, fps_frame_count);
                
                fps_frame_count = 0;
                fps_processed_count = 0;
                fps_start_time = now;
            }

            
            if (sf->seq != last_logged_seq && (sf->seq % 30 == 0)) {
                last_logged_seq = sf->seq;
                LG_INFO("inference_worker: seq=%llu detections=%d (%s)",
                        (unsigned long long)sf->seq, outs.num_dets,
                        config.model_name.c_str());
            }
            #endif
        }

        LG_INFO("inference_worker: stop (%s)", config.model_name.c_str());

    } catch (const std::exception& e) {
        LG_CRIT("inference_worker: exception: %s", e.what());
        std::fflush(nullptr);
        std::abort();
    } catch (...) {
        LG_CRIT("inference_worker: unknown exception");
        std::fflush(nullptr);
        std::abort();
    }
}

} // namespace inference_worker
