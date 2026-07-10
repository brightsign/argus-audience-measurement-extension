#include "models/model_runner_yolox.h"
#include "models/rga_color.h"
#include "metrics/log_global.h"
#include <cstring>
#include <chrono>
#include <cstdlib>

extern "C" {
  #include "yolox.h"
  #include "common.h"
  #include "image_utils.h"
  #include "postprocess.h"
  #include "file_utils.h"
}

using Clock = std::chrono::steady_clock;
static inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           Clock::now().time_since_epoch()).count();
}

/**
 * RKNNYoloXRunner: YOLOX Object Detector (Rockchip NPU backend)
 *
 * Real RKNN implementation using patterns from object_latest repo.
 * Runs inference on NPU core 1 (configurable).
 */

struct RKNNYoloXRunner::Impl {
    rknn_app_context_t ctx{};      // RKNN context (model state)
    bool loaded{false};
    
    image_buffer_t in_img{};       // Input image buffer
    object_detect_result_list od_results{};  // Output detections
    
    // Owned buffers for safe RKNN input (no use-after-scope)
    int in_w{0}, in_h{0};
    std::vector<uint8_t> in_u8;  // tightly-packed RGB buffer owned by this runner
};

RKNNYoloXRunner::RKNNYoloXRunner() noexcept
    : p_(std::make_unique<Impl>()) {
    LG_INFO("RKNNYoloXRunner: constructor");
}

RKNNYoloXRunner::~RKNNYoloXRunner() {
    unload();
    LG_INFO("RKNNYoloXRunner: destructor");
}

bool RKNNYoloXRunner::load(const ModelSpec& spec) noexcept {
    spec_ = spec;

    LG_INFO("RKNNYoloXRunner::load: model_path=%s, npu_core=%d",
            spec_.model_path.c_str(), spec_.npu_core);

    if (spec_.model_path.empty()) {
        LG_ERROR("RKNNYoloXRunner: empty model_path");
        return false;
    }

    // Initialize YOLOX model using RKNN API
    // This calls init_yolox_model from yolox.cc
    int ret = init_yolox_model(spec_.model_path.c_str(), &p_->ctx, spec_.npu_core);
    if (ret != 0) {
        LG_ERROR("RKNNYoloXRunner: init_yolox_model failed (%d) path=%s",
                 ret, spec_.model_path.c_str());
        return false;
    }

    // Initialize owned input buffer
    p_->in_w = spec_.input_size.w;
    p_->in_h = spec_.input_size.h;
    p_->in_u8.resize(size_t(p_->in_w) * size_t(p_->in_h) * 3);
    LG_INFO("RKNNYoloXRunner: allocated input buffer %dx%d (RGB, %zu bytes)",
            p_->in_w, p_->in_h, p_->in_u8.size());

    // Self-test: verify RKNN path with a black frame before threads start
    {
        p_->in_u8.assign(size_t(p_->in_w) * size_t(p_->in_h) * 3, 0);  // black buffer
        std::memset(&p_->in_img, 0, sizeof(p_->in_img));
        p_->in_img.width         = p_->in_w;
        p_->in_img.height        = p_->in_h;
        p_->in_img.width_stride  = p_->in_w;
        p_->in_img.height_stride = p_->in_h;
        p_->in_img.format        = IMAGE_FORMAT_RGB888;
        p_->in_img.virt_addr     = p_->in_u8.data();
        p_->in_img.size          = size_t(p_->in_w) * size_t(p_->in_h) * 3;
        p_->in_img.fd            = -1;
        std::memset(&p_->od_results, 0, sizeof(p_->od_results));
        
        LG_INFO("RKNNYoloXRunner: self-test infer start (black frame %dx%d)", p_->in_w, p_->in_h);
        ret = inference_yolox_model(&p_->ctx, &p_->in_img, &p_->od_results, BOX_THRESH);
        LG_INFO("RKNNYoloXRunner: self-test infer done ret=%d count=%d", ret, p_->od_results.count);
        if (ret != 0) {
            LG_ERROR("RKNNYoloXRunner: self-test failed; aborting load");
            release_yolox_model(&p_->ctx);
            return false;
        }
    }

    // NPU core affinity could be set here if RKNN API supports it
    // For now, YOLOX will use default core assignment or the one passed via RKNN init

    p_->loaded = true;
    LG_INFO("RKNNYoloXRunner: successfully loaded %s", spec_.model_path.c_str());
    return true;
}

bool RKNNYoloXRunner::reshape(int new_w, int new_h) noexcept {
    LG_INFO("RKNNYoloXRunner::reshape: %dx%d (YOLOX typically fixed)", new_w, new_h);
    // YOLOX is usually fixed at 640x640, so this may be a no-op
    return true;
}

bool RKNNYoloXRunner::infer(const FrameView& in, InferenceOutputs& out) noexcept {
    if (!p_ || !p_->loaded) {
        LG_ERROR("RKNNYoloXRunner::infer: model not loaded");
        return false;
    }

    const int64_t t0 = now_ns();

    // ---- Input validation ----
    if (in.fmt != PixelFormat::RGB24 && in.fmt != PixelFormat::BGR24) {
        LG_ERROR("RKNNYoloXRunner: unsupported input format (%d); expected RGB24 (1) or BGR24 (2)", int(in.fmt));
        return false;
    }
    if (in.width != p_->in_w || in.height != p_->in_h) {
        LG_ERROR("RKNNYoloXRunner: size mismatch (got %dx%d, want %dx%d)",
                 in.width, in.height, p_->in_w, p_->in_h);
        return false;
    }
    if (!in.plane0) {
        LG_ERROR("RKNNYoloXRunner: null input plane");
        return false;
    }
    const int row_bytes = p_->in_w * 3;
    if (in.stride0 < row_bytes) {
        LG_ERROR("RKNNYoloXRunner: bad stride0 (%d < %d)", in.stride0, row_bytes);
        return false;
    }

    // ---- Copy into owned, tightly-packed RGB buffer (W*H*3) ----
    uint8_t* dst = p_->in_u8.data();
    const uint8_t* src = in.plane0;
    const bool src_is_bgr = (in.fmt == PixelFormat::BGR24);

    if (in.stride0 == row_bytes) {
        // Fast path: source is contiguous. Offload BGR->RGB to RGA hardware
        // (near-zero CPU); RGB stays a straight copy.
        if (src_is_bgr) {
            model_pre::bgr_to_rgb_packed(src, dst, p_->in_w, p_->in_h);
        } else {
            std::memcpy(dst, src, size_t(row_bytes) * size_t(p_->in_h));
        }
    } else {
        // Strided fallback: per-row copy / scalar swap.
        for (int y = 0; y < p_->in_h; ++y) {
            const uint8_t* s = src + y * in.stride0;
            uint8_t* d       = dst + y * row_bytes;

            if (!src_is_bgr) {
                // RGB → memcpy row (no swap)
                std::memcpy(d, s, size_t(row_bytes));
            } else {
                // BGR → RGB swap per pixel (essential for RKNN)
                for (int x = 0; x < p_->in_w; ++x) {
                    const uint8_t b = s[3*x + 0];
                    const uint8_t g = s[3*x + 1];
                    const uint8_t r = s[3*x + 2];
                    d[3*x + 0] = r;
                    d[3*x + 1] = g;
                    d[3*x + 2] = b;
                }
            }
        }
    }
    pre_ns_ = now_ns() - t0;

    // ---- Prepare input image buffer for RKNN (owned by us) ----
    std::memset(&p_->in_img, 0, sizeof(p_->in_img));
    p_->in_img.width         = p_->in_w;
    p_->in_img.height        = p_->in_h;
    p_->in_img.width_stride  = p_->in_w;                       // REQUIRED
    p_->in_img.height_stride = p_->in_h;                       // REQUIRED
    p_->in_img.format        = IMAGE_FORMAT_RGB888;            // RGB888 format
    p_->in_img.virt_addr     = p_->in_u8.data();               // owned CPU buffer pointer
    p_->in_img.size          = size_t(p_->in_w) * size_t(p_->in_h) * 3;  // W*H*3
    p_->in_img.fd            = -1;                             // CPU buffer (not dma-buf)

    // ---- Run YOLOX inference ----
    // This calls inference_yolox_model from yolox.cc, which:
    // 1. Letterbox resizes to 640x640
    // 2. Sets RKNN input
    // 3. Runs inference via rknn_run()
    // 4. Retrieves outputs
    // 5. Post-processes with NMS and confidence filtering
    std::memset(&p_->od_results, 0, sizeof(p_->od_results));
    
    const float conf_threshold = BOX_THRESH;  // 0.25 from yolox.h
    int ret = inference_yolox_model(&p_->ctx, &p_->in_img, &p_->od_results, conf_threshold);
    if (ret != 0) {
        LG_ERROR("RKNNYoloXRunner: inference_yolox_model failed (%d)", ret);
        return false;
    }

    // Force fence: ensure RKNN has finished reading the input buffer before we return
    std::memset(&p_->in_img, 0, sizeof(p_->in_img));

    int64_t infer_end = now_ns();
    infer_ns_ = infer_end - t0 - pre_ns_;

    // ---- Convert YOLOX output to generic Detection array ----
    // Map object_detect_result_list to Detection vector
    // V6.2.3.2: CRITICAL - De-letterbox coordinates back to original frame space
    // YOLOX runs on 640x640 letterbox input, but tracker needs original camera coords
    dets_.clear();
    dets_.reserve(p_->od_results.count);
    
    // V6.2.3.5.5: Use ACTUAL model dimensions from RKNN, not hardcoded 640x640!
    // The model input is 320x320 (read from spec_.input_size during load())
    const int orig_w = (in.orig_width > 0) ? in.orig_width : in.width;
    const int orig_h = (in.orig_height > 0) ? in.orig_height : in.height;
    const int model_w = p_->in_w;  // Actual RKNN model input width (e.g., 320)
    const int model_h = p_->in_h;  // Actual RKNN model input height (e.g., 320)
    
    // Compute letterbox parameters used during preprocessing
    // For 640x480 camera -> 320x320 model: scale=0.5, letterbox=320x240, pad=(0,40)
    const float scale = std::min(float(model_w) / orig_w, float(model_h) / orig_h);
    const int letterbox_w = int(orig_w * scale);
    const int letterbox_h = int(orig_h * scale);
    const int pad_x = (model_w - letterbox_w) / 2;
    const int pad_y = (model_h - letterbox_h) / 2;
    
    // V6.2.3.2: Debug logging (first 3 frames only)
    static int debug_deletter_count = 0;
    if (debug_deletter_count < 3 && p_->od_results.count > 0) {
        LG_INFO("[YOLOX] De-letterbox: orig=%dx%d model=%dx%d scale=%.3f letterbox=%dx%d pad=(%d,%d)",
                orig_w, orig_h, model_w, model_h, scale, letterbox_w, letterbox_h, pad_x, pad_y);
        debug_deletter_count++;
    }
    
    for (int i = 0; i < p_->od_results.count; ++i) {
        const auto& obj_det = p_->od_results.results[i];
        
        // Boxes from RKNN are in letterboxed 640x640 space
        float x0_letter = static_cast<float>(obj_det.box.left);
        float y0_letter = static_cast<float>(obj_det.box.top);
        float x1_letter = static_cast<float>(obj_det.box.right);
        float y1_letter = static_cast<float>(obj_det.box.bottom);
        
        // De-letterbox: remove padding, scale back to original frame
        Detection d;
        d.x0 = (x0_letter - pad_x) / scale;
        d.y0 = (y0_letter - pad_y) / scale;
        d.x1 = (x1_letter - pad_x) / scale;
        d.y1 = (y1_letter - pad_y) / scale;
        
        // V6.2.3.5.2: Debug - log what we're getting from RKNN BEFORE assignment
        if (debug_deletter_count <= 3 && i < 2) {
            LG_INFO("[YOLOX-DEBUG] Det#%d: FROM RKNN: cls_id=%d prop=%.2f name='%s'",
                    i, obj_det.cls_id, obj_det.prop, obj_det.name);
        }
        
        // Set class_id and score FIRST (before any logging or clamping)
        d.score = obj_det.prop;
        d.class_id = obj_det.cls_id;
        
        // V6.2.3.2: Debug first few detections to verify de-letterbox
        if (debug_deletter_count <= 3 && i < 2) {
            LG_INFO("[YOLOX] Det#%d: letterbox=(%.1f,%.1f,%.1f,%.1f) -> camera=(%.1f,%.1f,%.1f,%.1f) class=%d score=%.2f",
                    i, x0_letter, y0_letter, x1_letter, y1_letter,
                    d.x0, d.y0, d.x1, d.y1, d.class_id, d.score);
        }
        
        // Clamp to original frame bounds (handle edge cases)
        d.x0 = std::max(0.0f, std::min(float(orig_w), d.x0));
        d.y0 = std::max(0.0f, std::min(float(orig_h), d.y0));
        d.x1 = std::max(0.0f, std::min(float(orig_w), d.x1));
        d.y1 = std::max(0.0f, std::min(float(orig_h), d.y1));
        
        dets_.push_back(d);
    }

    // ---- Map results to output ----
    out.dets = dets_.data();
    out.num_dets = static_cast<int>(dets_.size());
    out.lms = nullptr;  // YOLOX doesn't produce landmarks
    out.num_lms = 0;

    post_ns_ = now_ns() - infer_end;
    return true;
}

void RKNNYoloXRunner::unload() noexcept {
    if (!p_ || !p_->loaded) return;
    
    LG_INFO("RKNNYoloXRunner::unload: releasing RKNN resources");
    
    // This calls release_yolox_model from yolox.cc, which:
    // 1. Frees input_attrs
    // 2. Frees output_attrs
    // 3. Calls rknn_destroy() to cleanup RKNN context
    release_yolox_model(&p_->ctx);
    
    p_->loaded = false;
    LG_INFO("RKNNYoloXRunner::unload: complete");
}
