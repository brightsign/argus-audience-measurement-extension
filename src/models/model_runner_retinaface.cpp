#include "models/model_runner_retinaface.h"
#include "config/model_spec.h"
#include "metrics/log_global.h"
#include <chrono>
#include <cstring>

extern "C" {
  #include "retinaface.h" 
}

using Clock = std::chrono::steady_clock;
static inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           Clock::now().time_since_epoch()).count();
}

struct RKNNRetinafaceRunner::Impl {
  rknn_app_context_t ctx{};   // RKNN state lives here (hidden from header)
  bool loaded{false};

  // scratch that matches your C API expectations
  image_buffer_t in_img{};           // from the C header
  retinaface_result rf{};   // from the C header
  
  // Owned buffers for safe RKNN input (no use-after-scope)
  int in_w{0}, in_h{0};
  std::vector<uint8_t> in_u8;  // tightly-packed RGB buffer owned by this runner
};

RKNNRetinafaceRunner::RKNNRetinafaceRunner() noexcept
: p_(new Impl) {}

RKNNRetinafaceRunner::~RKNNRetinafaceRunner() {
  unload();
}

bool RKNNRetinafaceRunner::load(const ModelSpec& s) noexcept {
  LG_INFO("retinaface:load function ");
  spec_ = s;
  if (spec_.model_path.empty()) {
    LG_ERROR("retinaface:empty model_path");
    return false;
  }
  LG_INFO("retinaface: init retinaface model ");
  LG_INFO("retinaface: loaded model from %s", spec_.model_path.c_str());
  int ret = init_retinaface_model(spec_.model_path.c_str(), &p_->ctx, spec_.npu_core);
  if (ret != 0) {
    LG_ERROR("retinaface:init_retinaface_model failed (%d) path=%s", ret, spec_.model_path.c_str());
    return false;
  }
  
  // Initialize owned input buffer
  p_->in_w = spec_.input_size.w;
  p_->in_h = spec_.input_size.h;
  p_->in_u8.resize(size_t(p_->in_w) * size_t(p_->in_h) * 3);
  LG_INFO("retinaface: allocated input buffer %dx%d (RGB, %zu bytes)", 
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
    std::memset(&p_->rf, 0, sizeof(p_->rf));
    
    LG_INFO("retinaface: self-test infer start (black frame %dx%d)", p_->in_w, p_->in_h);
    int ret = inference_retinaface_model(&p_->ctx, &p_->in_img, &p_->rf);
    LG_INFO("retinaface: self-test infer done ret=%d count=%d", ret, p_->rf.count);
    if (ret != 0) {
      LG_ERROR("retinaface: self-test failed; aborting load");
      release_retinaface_model(&p_->ctx);
      return false;
    }
  }
  
  p_->loaded = true;
  LG_INFO("retinaface: loaded %s", spec_.model_path.c_str());
  return true;
}

void RKNNRetinafaceRunner::unload() noexcept {
  if (!p_ || !p_->loaded) return;
  release_retinaface_model(&p_->ctx);
  p_->loaded = false;
}

bool RKNNRetinafaceRunner::reshape(int new_w, int new_h) noexcept {
  // If your RKNN graph supports dynamic shapes, do it here.
  // Otherwise just update spec_ so orchestrator/pipeline can re-preprocess.
  spec_.input_size = { new_w, new_h };
  return true;
}

bool RKNNRetinafaceRunner::infer(const FrameView& in, InferenceOutputs& out) noexcept {
  if (!p_ || !p_->loaded) return false;
  const int64_t t0 = now_ns();

  // ---- Validate input ----
  if (in.fmt != PixelFormat::RGB24 && in.fmt != PixelFormat::BGR24) {
    LG_ERROR("retinaface: unsupported fmt=%d (expect RGB24/BGR24)", int(in.fmt));
    return false;
  }
  if (in.width != p_->in_w || in.height != p_->in_h) {
    LG_ERROR("retinaface: size mismatch (got %dx%d, want %dx%d)",
             in.width, in.height, p_->in_w, p_->in_h);
    return false;
  }
  if (!in.plane0) {
    LG_ERROR("retinaface: null input plane");
    return false;
  }
  const int row_bytes = p_->in_w * 3;
  if (in.stride0 < row_bytes) {
    LG_ERROR("retinaface: bad stride0 (%d < %d)", in.stride0, row_bytes);
    return false;
  }

  // ---- Copy into owned, tightly-packed RGB888 buffer ----
  uint8_t* dst = p_->in_u8.data();
  const uint8_t* src = in.plane0;
  const bool src_is_bgr = (in.fmt == PixelFormat::BGR24);

  for (int y = 0; y < p_->in_h; ++y) {
    const uint8_t* s = src + y * in.stride0;
    uint8_t* d       = dst + y * row_bytes;

    if (!src_is_bgr) {
      std::memcpy(d, s, size_t(row_bytes));           // RGB -> RGB
    } else {
      for (int x = 0; x < p_->in_w; ++x) {            // BGR -> RGB swap
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

  // ---- Prepare vendor image buffer (ALL REQUIRED FIELDS!) ----
  std::memset(&p_->in_img, 0, sizeof(p_->in_img));
  p_->in_img.width         = p_->in_w;
  p_->in_img.height        = p_->in_h;
  p_->in_img.width_stride  = p_->in_w;                       // REQUIRED: pixels per row
  p_->in_img.height_stride = p_->in_h;                       // REQUIRED: num rows
  p_->in_img.format        = IMAGE_FORMAT_RGB888;            // RGB888 format
  p_->in_img.virt_addr     = p_->in_u8.data();               // owned CPU buffer pointer
  p_->in_img.size          = size_t(p_->in_w) * size_t(p_->in_h) * 3;  // total bytes: W*H*3
  p_->in_img.fd            = -1;                             // CPU buffer (not dma-buf)

  // ---- Zero vendor result struct (contains fixed array object[128]) ----
  std::memset(&p_->rf, 0, sizeof(p_->rf));

  // ---- Run RKNN ----
  int ret = inference_retinaface_model(&p_->ctx, &p_->in_img, &p_->rf);
  if (ret != 0) {
    LG_ERROR("retinaface: inference_retinaface_model failed (%d)", ret);
    return false;
  }
  infer_ns_ = now_ns() - t0 - pre_ns_;

  // ---- Map to generic outputs (into our owned vectors) ----
  const int n = std::max(0, std::min(p_->rf.count, 128));
  dets_.resize(n);
  lms_.resize(n);
  for (int i = 0; i < n; ++i) {
    const retinaface_object_t& o = p_->rf.object[i];
    Detection d{};
    d.x0 = float(o.box.left);
    d.y0 = float(o.box.top);
    d.x1 = float(o.box.right);
    d.y1 = float(o.box.bottom);
    d.score = o.score;
    d.class_id = 0;  // face
    dets_[i] = d;

    Landmarks lm{};
    for (int k = 0; k < 5; ++k) {
      lm.pts[2*k+0] = float(o.ponit[k].x);
      lm.pts[2*k+1] = float(o.ponit[k].y);
    }
    lms_[i] = lm;
  }

  out.dets     = dets_.empty() ? nullptr : dets_.data();
  out.num_dets = int(dets_.size());
  out.lms      = lms_.empty() ? nullptr : lms_.data();
  out.num_lms  = int(lms_.size());

  post_ns_ = now_ns() - t0 - pre_ns_ - infer_ns_;
  return true;
}

const void* RKNNRetinafaceRunner::get_last_result() const noexcept {
  if (!p_) return nullptr;
  return static_cast<const void*>(&p_->rf);
}