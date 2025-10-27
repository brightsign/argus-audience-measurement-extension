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
  int ret = init_retinaface_model(spec_.model_path.c_str(), &p_->ctx);
  if (ret != 0) {
    LG_ERROR("retinaface:init_retinaface_model failed (%d) path=%s", ret, spec_.model_path.c_str());
    return false;
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

  // ---- Input mapping: accept RGB24 only for now ----
  if (in.fmt != PixelFormat::RGB24) {
    LG_ERROR("retinaface:unsupported input fmt (%d); expected RGB24", int(in.fmt));
    return false;
  }

  std::memset(&p_->in_img, 0, sizeof(p_->in_img));
  p_->in_img.width     = in.width;
  p_->in_img.height    = in.height;
  p_->in_img.virt_addr = in.plane0;                 // contiguous RGB bytes
  p_->in_img.size      = in.height * in.stride0;
  p_->in_img.format    = IMAGE_FORMAT_RGB888;       // BGR888 does not exist in your enum

  pre_ns_ = now_ns() - t0;

  // ---- Run RKNN ----
  std::memset(&p_->rf, 0, sizeof(p_->rf));
  int ret = inference_retinaface_model(&p_->ctx, &p_->in_img, &p_->rf);
  if (ret != 0) {
    LG_ERROR("retinaface:inference_retinaface_model failed (%d)", ret);
    return false;
  }
  infer_ns_ = now_ns() - t0 - pre_ns_;

  // ---- Map results -> InferenceOutputs ----
  // Map retinaface_result to the generic Detection array
  // Note: This is a non-owning reference - data lives in p_->rf
  out.num_dets = p_->rf.count;
  out.dets = nullptr;  // TODO: Create Detection array from retinaface_object_t array if needed for drawing
  out.num_lms = p_->rf.count * 5;  // Each face has 5 landmarks
  out.lms = nullptr;   // TODO: Create Landmarks array from retinaface_object_t landmarks if needed

  post_ns_ = now_ns() - t0 - pre_ns_ - infer_ns_;
  return true;
}

const void* RKNNRetinafaceRunner::get_last_result() const noexcept {
  if (!p_) return nullptr;
  return static_cast<const void*>(&p_->rf);
}