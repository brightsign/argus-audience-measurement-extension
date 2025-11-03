#include "models/model_runner_yolox.h"
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
    dets_.clear();
    dets_.reserve(p_->od_results.count);
    
    for (int i = 0; i < p_->od_results.count; ++i) {
        const auto& obj_det = p_->od_results.results[i];
        
        Detection d;
        d.x0 = static_cast<float>(obj_det.box.left);
        d.y0 = static_cast<float>(obj_det.box.top);
        d.x1 = static_cast<float>(obj_det.box.right);
        d.y1 = static_cast<float>(obj_det.box.bottom);
        d.score = obj_det.prop;
        d.class_id = obj_det.cls_id;
        
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
