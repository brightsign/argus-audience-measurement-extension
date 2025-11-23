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
    if (skip_frames <= 1) return true;  // Process all frames
    return (seq & (skip_frames - 1)) == 0;  // Bitmask for power-of-2 skip
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
    
    // Use OpenCV for letterbox resize (RGA doesn't support offset/padding easily)
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
    // V6.2.3.2: Store original camera dimensions for de-letterboxing
    fv_in.orig_width  = sf->width;
    fv_in.orig_height = sf->height;
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
            if (!resize_frame_rga(sf->bgr, sf->width, sf->height,
                                  rgb_resized_buf, dst_w, dst_h)) {
                LG_WARN("inference_worker: RGA resize failed (seq=%llu, %s)",
                        (unsigned long long)sf->seq, config.model_name.c_str());
                continue;
            }

            // Prepare input frame view
            FrameView fv_in{};
            prepare_frame_view(fv_in, rgb_resized_buf, dst_w, dst_h, sf);

            // Run inference
            InferenceOutputs outs{};
            if (!runner->infer(fv_in, outs)) {
                LG_WARN("inference_worker: inference failed (seq=%llu, %s)",
                        (unsigned long long)sf->seq, config.model_name.c_str());
                continue;
            }

            // Store results in fusion output FIRST (before visualization)
            store_inference_results(runner, outs, fusion_output, sf->seq);

            // Draw overlays and save debug JPEG
            // V6.2.3.2: Pass original dimensions so visualization can scale coordinates
            // V6.2.3.5.7: Pass second_runner to draw detections from both models on same frame
            // V7.0.2: Pass fusion_output to access synchronized detection results
            cv::Mat rgb_mat(dst_h, dst_w, CV_8UC3, rgb_resized_buf.data());
            visualization::process_inference_results(runner, rgb_mat, debug_frame_idx,
                                                     sf->width, sf->height, second_runner,
                                                     fusion_output);

            // Write frame to disk if writer is available
            if (frame_writer) {
                PipelineResult result{};
                result.seq = sf->seq;
                frame_writer->writeFrame(rgb_mat, result);
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
