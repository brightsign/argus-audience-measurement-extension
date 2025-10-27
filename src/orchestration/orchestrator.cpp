#include <chrono>
#include <thread>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>
#include <cstdio>
#include "orchestration/orchestrator.h"
#include "input/input_factory.h"
#include "health/health_manager.h"
#include "metrics/file_logger.h"
#include "metrics/log_global.h"
#include "models/model_factory.h"
#include "models/model_runner.h"
#include "models/model_runner_retinaface.h"
#include "config/model_spec.h"
#include "attention.h"
#include "retinaface.h"

#include <rga/rga.h>
#include <rga/im2d.h>

#define DEBUG_FPS 1

namespace {
using Clock = std::chrono::steady_clock;
inline int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
}

namespace {

// Simple, correctness-first NV12 -> RGB (BT.601). Good enough for bring-up.
static void nv12_to_rgb_320x320(const FrameView& nv12, uint8_t* dst_rgb, int dst_w, int dst_h) {
  // nearest resize + convert. For bring-up, we'll just sample center-cropped
  const int src_w = nv12.width, src_h = nv12.height;
  const int strideY = nv12.stride0, strideUV = nv12.stride1;
  const uint8_t* Y = nv12.plane0;
  const uint8_t* UV = nv12.plane1;

  // compute a letterbox ROI (center fit)
  float sx = float(dst_w)/src_w, sy = float(dst_h)/src_h;
  float scale = (sx < sy) ? sx : sy;
  int w = int(src_w * scale), h = int(src_h * scale);
  int offx = (dst_w - w)/2, offy = (dst_h - h)/2;

  // Fill black frame
  std::fill(dst_rgb, dst_rgb + dst_w*dst_h*3, 0);

  for (int dy = 0; dy < h; ++dy) {
    int syi = int(dy / scale);
    const uint8_t* yrow = Y + syi * strideY;
    const uint8_t* uvrow = UV + (syi/2) * strideUV;
    for (int dx = 0; dx < w; ++dx) {
      int sxi = int(dx / scale);
      int Yv = yrow[sxi];
      int u = uvrow[(sxi/2)*2+0] - 128;
      int v = uvrow[(sxi/2)*2+1] - 128;
      // BT.601 approx
      int C = Yv - 16; if (C < 0) C = 0;
      int R = (298*C + 409*v + 128) >> 8;
      int G = (298*C - 100*u - 208*v + 128) >> 8;
      int B = (298*C + 516*u + 128) >> 8;
      if (R<0) R=0; if (R>255) R=255;
      if (G<0) G=0; if (G>255) G=255;
      if (B<0) B=0; if (B>255) B=255;

      int outx = offx + dx;
      int outy = offy + dy;
      uint8_t* o = dst_rgb + (outy * dst_w + outx) * 3;
      o[0] = (uint8_t)R; o[1] = (uint8_t)G; o[2] = (uint8_t)B;
    }
  }
}

static void nv12_to_rgb_letterbox(const FrameView& nv12,
                                  uint8_t* dst, int dst_w, int dst_h) {
  const int src_w = nv12.width, src_h = nv12.height;
  const int strideY = nv12.stride0, strideUV = nv12.stride1;
  const uint8_t* Y  = nv12.plane0;
  const uint8_t* UV = nv12.plane1;

  // compute letterbox ROI
  const float sx = float(dst_w)/src_w, sy = float(dst_h)/src_h;
  const float scale = (sx < sy) ? sx : sy;
  const int w = int(src_w * scale);
  const int h = int(src_h * scale);
  const int offx = (dst_w - w)/2, offy = (dst_h - h)/2;

  // black fill
  std::fill(dst, dst + size_t(dst_w)*dst_h*3, 0);

  for (int dy = 0; dy < h; ++dy) {
    const int syi = int(dy / scale);
    const uint8_t* yrow  = Y  + syi * strideY;
    const uint8_t* uvrow = UV + (syi/2) * strideUV;
    for (int dx = 0; dx < w; ++dx) {
      const int sxi = int(dx / scale);
      const int Yv = yrow[sxi];
      const int u = uvrow[(sxi/2)*2+0] - 128;
      const int v = uvrow[(sxi/2)*2+1] - 128;
      int C = Yv - 16; if (C < 0) C = 0;
      int R = (298*C + 409*v + 128) >> 8;
      int G = (298*C - 100*u - 208*v + 128) >> 8;
      int B = (298*C + 516*u + 128) >> 8;
      if (R<0) R=0; if (R>255) R=255;
      if (G<0) G=0; if (G>255) G=255;
      if (B<0) B=0; if (B>255) B=255;

      const int outx = offx + dx;
      const int outy = offy + dy;
      uint8_t* o = dst + (size_t(outy) * dst_w + outx) * 3;
      o[0] = (uint8_t)R; o[1] = (uint8_t)G; o[2] = (uint8_t)B; // RGB
    }
  }
}

// Normalize u8 RGB to float32 (RGB or BGR ordering) using ModelSpec::norm.
// mean/std arrays are taken as-is (assumed to match the expected channel order).
static void normalize_rgb_u8_to_float(const uint8_t* src_rgb, float* dst_f,
                                      int w, int h,
                                      const float mean[3],
                                      const float stdv[3],
                                      bool expect_bgr,
                                      float scale /* usually 1/255 */) {
  const int N = w * h;
  for (int i = 0; i < N; ++i) {
    const float r = src_rgb[3*i + 0] * scale;
    const float g = src_rgb[3*i + 1] * scale;
    const float b = src_rgb[3*i + 2] * scale;
    if (!expect_bgr) {
      dst_f[3*i + 0] = (r - mean[0]) / stdv[0];
      dst_f[3*i + 1] = (g - mean[1]) / stdv[1];
      dst_f[3*i + 2] = (b - mean[2]) / stdv[2];
    } else {
      // BGR expected: write in B,G,R order with corresponding mean/std
      dst_f[3*i + 0] = (b - mean[0]) / stdv[0];
      dst_f[3*i + 1] = (g - mean[1]) / stdv[1];
      dst_f[3*i + 2] = (r - mean[2]) / stdv[2];
    }
  }
}

} // namespace

namespace {

  // Draw bounding boxes, scores, and 5-point landmarks onto a BGR cv::Mat
  // and save it as a JPEG to `path`.
  static void save_debug_jpg(const cv::Mat& visFrame,
                                          const char* path,
                                          uint32_t frame_idx) noexcept
  {
      // Only dump, e.g., every 3rd frame to reduce I/O cost:
      if ((frame_idx % 3u) != 0u) {
        return;
      }

      try {
          cv::imwrite(path, visFrame); // visFrame already annotated
      } catch (...) {}

  } // namespace
}

Orchestrator::Orchestrator(PipelineConfig cfg) noexcept
    : cfg_(std::move(cfg)),
      state_(OrchestratorState::Stopped),
      source_health_(detect_source_kind(cfg_.input)) {}

Orchestrator::~Orchestrator() { stop_worker(); destroy_pipeline(); }

bool Orchestrator::start() noexcept {
  if (state_.load(std::memory_order_acquire) != OrchestratorState::Stopped) return true;
  state_.store(OrchestratorState::Starting, std::memory_order_release);
  stop_.store(false, std::memory_order_release);

  LG_INFO("Build pipeline from orchestrator");
  if (!build_pipeline()) { state_.store(OrchestratorState::Error, std::memory_order_release); return false; }
  if (!start_worker())   { destroy_pipeline(); state_.store(OrchestratorState::Error, std::memory_order_release); return false; }

  LG_INFO("Create supervisor thread");
  supervisor_th_ = std::thread(&Orchestrator::supervisor_loop, this);
  state_.store(OrchestratorState::Running, std::memory_order_release);
  return true;
}

void Orchestrator::request_stop() noexcept { stop_.store(true, std::memory_order_release); }

void Orchestrator::join() noexcept {
  if (supervisor_th_.joinable()) supervisor_th_.join();
  if (worker_th_.joinable())     worker_th_.join();
}

bool Orchestrator::switch_input(const InputConfig& new_input) noexcept {
  cfg_.input = new_input;
  mark_broken(FaultCode::None, now_ns());
  return true;
}

bool Orchestrator::build_pipeline() noexcept {
  try { input_ = make_input(cfg_.input); } catch (...) { input_.reset(); }
  if (!input_) { LG_ERROR("[orch] failed to create input\n"); return false; }

  source_health_.reinit(detect_source_kind(cfg_.input));
  BackoffPolicy pol{}; pol.base_ms=250; pol.max_ms=8000; pol.factor=2.0f; pol.jitter_ms=100;
  source_health_.setBackoffPolicy(pol);

  if (!input_->open())  { LG_ERROR("[orch] input->open() failed\n");  source_health_.markBroken(); return false; }

  // Start capturing the frames
  if (!input_->start()) { LG_ERROR("[orch] input->start() failed\n"); input_->close(); source_health_.markBroken(); return false; }

  last_heartbeat_ns_.store(now_ns(), std::memory_order_relaxed);
  LG_INFO("make model runner\n");
  runner_ = make_model_runner(cfg_.model);
  if (!runner_) {
    LG_ERROR("[orch] failed to create model runner\n");
    return false;
  }
  LG_INFO("after make model runner:exit\n");
  LG_ERROR("[orch] model load path: %s\n", cfg_.model.model_path.c_str());
  
  if (!runner_->load(cfg_.model)) {
    LG_ERROR("[orch] model load failed: %s\n", cfg_.model.model_path.c_str());
    runner_.reset();
    return false;
  }
  return true;
}

void Orchestrator::destroy_pipeline() noexcept {
  if (input_) { input_->stop(); input_->close(); input_.reset(); }
  if (runner_) {
    runner_->unload();
    runner_.reset();
  }
}

bool Orchestrator::start_worker() noexcept {
  if (worker_th_.joinable()) return true;
  LG_INFO("start_worker:start worker loop thread\n");
  worker_th_ = std::thread(&Orchestrator::worker_loop, this);
  return true;
}

void Orchestrator::stop_worker() noexcept {
  stop_.store(true, std::memory_order_release);
  if (worker_th_.joinable()) worker_th_.join();
}

void Orchestrator::worker_loop() noexcept{
    LG_INFO("worker_loop", "USB fast path starting");

    constexpr int kIdleSleepMs = 2;
    auto heartbeat = [this]() noexcept { last_heartbeat_ns_.store(now_ns(), std::memory_order_release); };
    LG_INFO("worker_loop:load\n");
    heartbeat();  // Set initial heartbeat so supervisor doesn't think we're stalled before first frame

    using clock_t = std::chrono::steady_clock;
    auto t_start  = clock_t::now();
    int frame_count = 0;

    uint32_t debug_frame_idx = 0;

    const int dst_w = cfg_.model.input_size.w;
    const int dst_h = cfg_.model.input_size.h;

    const bool model_expects_rgb =
        (cfg_.model.input_layout == ColorLayout::RGB);

    while (!stop_.load(std::memory_order_relaxed)) {
        // 1. Fetch frame from camera into FrameView
        FrameView camView{};
        FetchStatus st = input_->tryFetch(camView);
        if (st != FetchStatus::Ok) {
            int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                clock_t::now().time_since_epoch()).count();
            source_health_.onBusError(now_ns, "usb fetch fail");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // mark source health OK
        source_health_.onFrameOk(camView.pts_ns);

        // camView.plane0 is BGR24 from UsbInputSource
        cv::Mat bgr_src(
            camView.height,
            camView.width,
            CV_8UC3,
            camView.plane0);

        // 2. Resize to model input size
        cv::Mat bgr_resized;
        cv::resize(
            bgr_src,
            bgr_resized,
            cv::Size(dst_w, dst_h),
            0, 0,
            cv::INTER_LINEAR
        );

        // 3. Convert to model layout if model wants RGB
        const uint8_t* model_input_ptr = nullptr;
        PixelFormat fmt_for_runner = PixelFormat::BGR24;
        cv::Mat model_mat;

        if (model_expects_rgb) {
            cv::cvtColor(bgr_resized, model_mat, cv::COLOR_BGR2RGB);
            model_input_ptr = model_mat.data;
            fmt_for_runner  = PixelFormat::RGB24;
        } else {
            model_mat = bgr_resized; // alias
            model_input_ptr = bgr_resized.data;
            fmt_for_runner  = PixelFormat::BGR24;
        }

        // 4. Build inference input view
        FrameView fv_in{};
        fv_in.fmt     = fmt_for_runner;
        fv_in.width   = dst_w;
        fv_in.height  = dst_h;
        fv_in.stride0 = dst_w * 3;
        fv_in.stride1 = 0;
        fv_in.plane0  = const_cast<uint8_t*>(model_input_ptr);
        fv_in.plane1  = nullptr;
        fv_in.pts_ns  = camView.pts_ns;

        InferenceOutputs outs{};
        bool ok_infer = (runner_ && runner_->infer(fv_in, outs));
        if (!ok_infer) {
            int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                clock_t::now().time_since_epoch()).count();
            source_health_.onBusError(now_ns, "inference fail");
        } else {
            auto* retinaface_runner =
            dynamic_cast<RKNNRetinafaceRunner*>(runner_.get());

            if (retinaface_runner) {
                // raw model-specific struct from your legacy pipeline
                const retinaface_result* result =
                    static_cast<const retinaface_result*>(
                        retinaface_runner->get_last_result());

                if (result && result->count > 0) {
                    int attending_total = 0;

                    // We'll draw directly on bgr_resized (BGR colors)
                    cv::Mat drawMat = bgr_resized; // alias, not copy

                    for (int i = 0; i < result->count; ++i) {
                        const auto& obj = result->object[i];

                        // choose box color: green if looking, red otherwise
                        bool attending = face_is_looking_at_us(obj);
                        if (attending) {
                            attending_total++;
                        }

                        cv::Scalar box_color = attending
                            ? cv::Scalar(0,255,0)     // green in BGR
                            : cv::Scalar(0,0,255);    // red in BGR

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
                        // (assuming obj.ponit[0..4] are {x,y} in resized frame coords)
                        for (int lm = 0; lm < 5; ++lm) {
                            int lx = (int)obj.ponit[lm].x;
                            int ly = (int)obj.ponit[lm].y;

                            // eye landmarks cyan-ish, others yellow-ish (cosmetic)
                            cv::Scalar lm_color = (lm < 2)
                                ? cv::Scalar(255,255,0)   // cyan-ish in BGR (blue+green)
                                : cv::Scalar(0,255,255);  // yellow-ish in BGR (green+red)

                            cv::circle(
                                drawMat,
                                cv::Point(lx, ly),
                                2,
                                lm_color,
                                2,
                                cv::LINE_AA
                            );
                        }

                        // optional: put "attn" label
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
                    #ifdef DEBUG_FPS
                    LG_INFO("overlay: faces=%d attending=%d",
                            result->count,
                            attending_total);
                    #endif
                }
            }
            // 4b. DEBUG OUTPUT
            // We annotate bgr_resized, which is in BGR color space and same W×H as fv_in.
            save_debug_jpg(/*visFrame=*/bgr_resized,
                                        /*path=*/"/tmp/output.jpg",
                                        /*frame_idx=*/debug_frame_idx);
            debug_frame_idx++;
        }

        // 5. FPS log (once per ~1s)
        frame_count++;
        auto t_now = clock_t::now();
        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_start).count();
        if (elapsed_ms >= 1000) {
            float fps = (elapsed_ms > 0)
                ? (1000.0f * frame_count / float(elapsed_ms))
                : 0.0f;

            LG_INFO("Performance: %.1f FPS | Frame %dx%d",
                    fps, dst_w, dst_h);

            frame_count = 0;
            t_start = t_now;
        }

        // 6. Heartbeat for supervisor
        heartbeat();
    }

    LG_INFO("worker_loop", "USB fast path exiting");
}

void Orchestrator::supervisor_loop() noexcept {
  // Use configured heartbeat timeout, or default based on input type
  // USB cameras are slow (5fps = 200ms per frame), RTSP should be faster
  // Set a reasonable default: 3 seconds for USB/slow sources, allow override
  int heartbeat_timeout_ms = (cfg_.heartbeat_timeout_ms > 0) ? cfg_.heartbeat_timeout_ms : 3000;
  
  // If heartbeat_timeout_ms was set to 1000 (old hardcoded value), increase it for USB
  if (heartbeat_timeout_ms <= 1000 && 
      (cfg_.input.usb_device == "/dev/video1" || !cfg_.input.usb_device.empty())) {
    heartbeat_timeout_ms = 3000;  // USB cameras need more time
    LG_INFO("supervisor_loop:detected USB source, increasing timeout to %dms\n", heartbeat_timeout_ms);
  }
  
  LG_INFO("supervisor_loop:load (heartbeat_timeout_ms=%d)\n", heartbeat_timeout_ms);
  while (!stop_.load(std::memory_order_acquire)) {
    const int64_t now = now_ns();
    const int64_t last = last_heartbeat_ns_.load(std::memory_order_acquire);
    const int64_t age_ms = (last>0) ? (now-last)/1'000'000 : 0;
    if (last && age_ms > heartbeat_timeout_ms) {
      LG_WARN("supervisor_loop:heartbeat stale (age_ms=%lld > timeout=%d)\n", age_ms, heartbeat_timeout_ms);
      source_health_.onAppsinkStarvation(now);
      source_health_.markBroken();
    }
    if (source_health_.isBroken()) {
      state_.store(OrchestratorState::Recovering, std::memory_order_release);
      LG_INFO("supervisor_loop:recover_pipeline (reason: health broken)\n");
      (void)recover_pipeline(now);
    } else if (state_.load(std::memory_order_acquire) == OrchestratorState::Recovering) {
      state_.store(OrchestratorState::Running, std::memory_order_release);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

void Orchestrator::mark_broken(FaultCode, int64_t now) noexcept {
  source_health_.onNoFrames(now);
  source_health_.markBroken();
}

bool Orchestrator::recover_pipeline(int64_t now) noexcept {
  stop_worker();
  destroy_pipeline();
  const int delay_ms = source_health_.nextBackoffMs(now);
  source_health_.noteRecoveryAttempt(now);
  if (delay_ms>0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  if (!build_pipeline()) return false;
  if (!start_worker())   { destroy_pipeline(); return false; }
  source_health_.clearBroken();
  last_heartbeat_ns_.store(now_ns(), std::memory_order_release);
  return true;
}
