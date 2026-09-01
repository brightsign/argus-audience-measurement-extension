#include <chrono>
#include <thread>
#include <iostream>
#include <unordered_set>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include "orchestration/orchestrator.h"
#include "orchestration/visualization.h"
#include "orchestration/inference_worker.h"
#include "input/input_factory.h"
#include "input/input_usb.h"
#include "input/registry_helper.h"
#include "health/health_manager.h"
#include "metrics/file_logger.h"
#include "metrics/log_global.h"
#include "models/model_factory.h"
#include "models/model_runner.h"
#include "models/model_runner_retinaface.h"
#include "models/model_runner_yolox.h"
#include "pipeline/shared_frame.h"
#include "pipeline/frame_mailbox.h"
#include "pipeline/pipeline_types.h"
#include "config/model_spec.h"
#include "attention.h"
#include "retinaface.h"
#include "image_utils.h"
#include "common/device_info.h"
#include "common.h"
#include "output/publisher_factory.h"



namespace {
using Clock = std::chrono::steady_clock;
inline int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
}

// ...

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

Orchestrator::Orchestrator(PipelineConfig cfg) noexcept
    : cfg_(std::move(cfg)),
      state_(OrchestratorState::Stopped),
      source_health_(detect_source_kind(cfg_.input)) {
  // Remember the input exactly as configured. recover_pipeline() mutates
  // cfg_.input (it may overwrite rtsp_url with a registry USB node), so we keep
  // a pristine copy to reconnect explicit RTSP/HTTP/file sources to themselves.
  // Copying std::string fields may allocate; this ctor is noexcept, so make the
  // snapshot best-effort and fall back to an empty InputConfig on OOM rather
  // than letting an exception escape and call std::terminate.
  try {
    original_input_ = cfg_.input;
  } catch (...) {
    original_input_ = InputConfig{};
  }
}

Orchestrator::~Orchestrator() { stop_threads(); destroy_pipeline(); }

bool Orchestrator::start() noexcept {
  if (state_.load(std::memory_order_acquire) != OrchestratorState::Stopped) return true;
  state_.store(OrchestratorState::Starting, std::memory_order_release);
  orchestrator_stop_.store(false, std::memory_order_release);

  LG_INFO("Build pipeline from orchestrator");
  if (!build_pipeline()) { state_.store(OrchestratorState::Error, std::memory_order_release); return false; }
  if (!start_threads_after_build())   { destroy_pipeline(); state_.store(OrchestratorState::Error, std::memory_order_release); return false; }

  LG_INFO("Create supervisor thread");
  supervisor_th_ = std::thread(&Orchestrator::supervisor_loop, this);
  state_.store(OrchestratorState::Running, std::memory_order_release);
  return true;
}

void Orchestrator::request_stop() noexcept { orchestrator_stop_.store(true, std::memory_order_release); }

void Orchestrator::join() noexcept {
  if (supervisor_th_.joinable()) supervisor_th_.join();
  if (capture_th_.joinable())    capture_th_.join();
  if (face_th_.joinable())       face_th_.join();
  if (yolo_th_.joinable())       yolo_th_.join();
}

bool Orchestrator::switch_input(const InputConfig& new_input) noexcept {
  cfg_.input = new_input;
  mark_broken(FaultCode::None, now_ns());
  return true;
}

bool Orchestrator::build_pipeline() noexcept {
  std::unique_ptr<IInputSource> input_tmp;
  try { input_tmp = make_input(cfg_.input); } catch (...) { input_tmp.reset(); }
  if (!input_tmp) { LG_ERROR("[orch] failed to create input\n"); return false; }

  source_health_.reinit(detect_source_kind(cfg_.input));
  BackoffPolicy pol{}; pol.base_ms=250; pol.max_ms=8000; pol.factor=2.0f; pol.jitter_ms=100;
  source_health_.setBackoffPolicy(pol);
  LG_INFO("Open input source\n");
  if (!input_tmp->open())  { LG_ERROR("[orch] input->open() failed\n");  source_health_.markBroken(); return false; }

  LG_INFO("Start capturing frames\n");
  // Start capturing the frames
  if (!input_tmp->start()) { LG_ERROR("[orch] input->start() failed\n"); input_tmp->close(); source_health_.markBroken(); return false; }

  // Get first frame to determine frame size for tracker ROI
  FrameView first_frame{};
  FetchStatus st = input_tmp->tryFetch(first_frame);
  if (st == FetchStatus::Ok && first_frame.width > 0 && first_frame.height > 0) {
    // V7.1: CRITICAL FIX - Use ORIGINAL source frame dimensions, not model input size!
    // first_frame.width/height = 320x320 (model input after resize)
    // first_frame.orig_width/orig_height = 1280x720 (actual source from RTSP/USB/file)
    // Tracker and MQTT publisher need SOURCE dimensions because detections are de-letterboxed
    const int tracker_w = (first_frame.orig_width > 0) ? first_frame.orig_width : first_frame.width;
    const int tracker_h = (first_frame.orig_height > 0) ? first_frame.orig_height : first_frame.height;
    
    person_tracker_.set_frame_size(tracker_w, tracker_h);
    // V6.2: Store frame dimensions in fusion state for normalized speed
    {
      std::lock_guard<std::mutex> lk(fusion_.m);
      fusion_.frame_width = tracker_w;
      fusion_.frame_height = tracker_h;
    }
    LG_INFO("[orch] Tracker frame size set: %dx%d (source), model input: %dx%d\n", 
            tracker_w, tracker_h, first_frame.width, first_frame.height);
  } else {
    // Fallback to default if fetch fails
    person_tracker_.set_frame_size(640, 480);
    {
      std::lock_guard<std::mutex> lk(fusion_.m);
      fusion_.frame_width = 640;
      fusion_.frame_height = 480;
    }
    LG_WARN("[orch] Could not fetch frame size, using default 640x480\n");
  }

  last_heartbeat_ns_.store(now_ns(), std::memory_order_relaxed);
  LG_INFO("make model runners (primary: face, secondary: yolo)\n");
  
  // Set enable flags from config
  enable_face_model_ = cfg_.enable_face_model;
  enable_yolo_model_ = cfg_.enable_yolo_model;
  LG_INFO("[orch] Inference pipeline: enable_face_model=%s enable_yolo_model=%s\n",
          enable_face_model_ ? "true" : "false",
          enable_yolo_model_ ? "true" : "false");
  
  // Create face runner (RetinaFace on NPU core 0) - only if enabled
  if (enable_face_model_) {
    std::unique_ptr<IModelRunner> face_runner_tmp = make_model_runner(cfg_.primary_model);
    if (!face_runner_tmp) {
      LG_ERROR("[orch] failed to create face runner\n");
      return false;
    }
    
    LG_INFO("[orch] face model load path: %s (core %d)\n", cfg_.primary_model.model_path.c_str(), cfg_.primary_model.npu_core);
    if (!face_runner_tmp->load(cfg_.primary_model)) {
      LG_ERROR("[orch] face model load failed: %s\n", cfg_.primary_model.model_path.c_str());
      face_runner_tmp.reset();
      return false;
    }
    face_runner_ = std::shared_ptr<IModelRunner>(std::move(face_runner_tmp));
  } else {
    LG_INFO("[orch] RetinaFace disabled (test_yolo_only mode)\n");
    face_runner_.reset();
  }
  
  // Create yolo runner (YOLOX on NPU) - only if enabled
  if (enable_yolo_model_) {
    std::unique_ptr<IModelRunner> yolo_runner_tmp = make_model_runner(cfg_.secondary_model);
    if (!yolo_runner_tmp) {
      LG_ERROR("[orch] failed to create yolo runner\n");
      return false;
    }
    
    LG_INFO("[orch] yolo model load path: %s\n", cfg_.secondary_model.model_path.c_str());
    if (!yolo_runner_tmp->load(cfg_.secondary_model)) {
      LG_ERROR("[orch] yolo model load failed: %s\n", cfg_.secondary_model.model_path.c_str());
      yolo_runner_tmp.reset();
      return false;
    }
    yolo_runner_ = std::shared_ptr<IModelRunner>(std::move(yolo_runner_tmp));
  } else {
    LG_INFO("[orch] YOLOX disabled (test_yolo_only mode)\n");
    yolo_runner_.reset();
  }
  
  // Create separate frame writers for each model (both write to same dir)
  if (cfg_.enable_frame_output && !cfg_.output_dir.empty()) {
    try {
      // V6.2.3.5.7: Face writer DISABLED - YOLOX worker will draw both models' detections
      frame_writer_face_ = make_frame_writer_null();
      LG_INFO("[orch] Face frame writer DISABLED (YOLOX worker draws combined output)\n");
      
      // V6.2.3.5.7: YOLOX writer enabled - draws both face + person detections
      frame_writer_yolo_ = make_frame_writer_disk(cfg_.output_dir, cfg_.max_frames, cfg_.frame_quality, cfg_.blur_config);
      LG_INFO("[orch] YOLO frame writer enabled: output_dir=%s max_frames=%d quality=%d blur=%s\n",
              cfg_.output_dir.c_str(), cfg_.max_frames, cfg_.frame_quality,
              cfg_.blur_config.enabled ? "enabled" : "disabled");
    } catch (const std::exception& e) {
      LG_WARN("[orch] Failed to create frame writers: %s (will continue without output)\n", e.what());
      frame_writer_face_.reset();
      frame_writer_yolo_.reset();
    }
  } else {
    frame_writer_face_ = make_frame_writer_null();
    frame_writer_yolo_ = make_frame_writer_null();
    LG_INFO("[orch] Frame writers disabled (using null writers)\n");
  }
  
  // Convert input to shared_ptr and assign to member variable
  input_  = std::shared_ptr<IInputSource>(std::move(input_tmp));
  
  // Initialize device and stream identifiers before creating publishers
  // Get device ID from config, or auto-detect from system (MAC address, serial, hostname)
  if (!cfg_.device_id.empty()) {
    device_id_ = cfg_.device_id;
    LG_INFO("[orch] Using device ID from config: %s\n", device_id_.c_str());
  } else {
    device_id_ = device_info::get_device_id("BS-UNKNOWN");
    LG_INFO("[orch] Auto-detected device ID: %s\n", device_id_.c_str());
  }
  
  stream_id_ = cfg_.input.usb_device.empty() ? 
               cfg_.input.rtsp_url : cfg_.input.usb_device;
  if (stream_id_.empty() && !cfg_.input.file_path.empty()) {
    stream_id_ = cfg_.input.file_path;
  }
  
  // Create and start publishers from configuration
  if (!cfg_.publishers.empty()) {
    LG_INFO("[orch] Creating %zu publisher(s) from configuration\n", cfg_.publishers.size());
    auto pubs = make_async_publishers(cfg_.publishers, 64, device_id_, stream_id_);
    for (auto& p : pubs) {
      if (p && p->start()) {
        publishers_.push_back(std::move(p));
        LG_INFO("[orch] Started publisher successfully\n");
      } else {
        LG_WARN("[orch] Failed to start publisher\n");
      }
    }
    LG_INFO("[orch] Active publishers: %zu\n", publishers_.size());
  } else {
    LG_INFO("[orch] No publishers configured\n");
  }
  
  // Log tracker initialization
  LG_INFO("[orch] Person tracker initialized (device=%s stream=%s)\n",
          device_id_.c_str(), stream_id_.c_str());
  
  return true;
}

void Orchestrator::destroy_pipeline() noexcept {
  if (input_) { input_->stop(); input_->close(); input_.reset(); }
  if (face_runner_) {
    face_runner_->unload();
    face_runner_.reset();
  }
  if (yolo_runner_) {
    yolo_runner_->unload();
    yolo_runner_.reset();
  }
  if (frame_writer_face_) {
    frame_writer_face_->flush();
    frame_writer_face_.reset();
  }
  if (frame_writer_yolo_) {
    frame_writer_yolo_->flush();
    frame_writer_yolo_.reset();
  }
  
  // Stop and clear publishers
  for (auto& pub : publishers_) {
    if (pub) {
      pub->stop();
    }
  }
  publishers_.clear();
}

bool Orchestrator::start_threads_after_build() noexcept {
  if (capture_th_.joinable() || face_th_.joinable() || yolo_th_.joinable()) {
    LG_WARN("start_threads_after_build: threads already running\n");
    return true;
  }
  
  LG_INFO("start_threads_after_build: launching capture + model threads\n");
  LG_INFO("  enable_face_model=%s enable_yolo_model=%s\n",
          enable_face_model_ ? "true" : "false",
          enable_yolo_model_ ? "true" : "false");
  
  // Reset stop flags
  stop_capture_.store(false, std::memory_order_release);
  stop_face_.store(false, std::memory_order_release);
  stop_yolo_.store(false, std::memory_order_release);
  
  try {
    // Launch the capture thread (always runs)
    LG_INFO("start_threads_after_build: launching capture thread\n");
    capture_th_ = std::thread([this]() {
      try {
        capture_loop_threadfn();
      } catch (const std::exception& e) {
        LG_CRIT("capture thread crashed: %s\n", e.what());
        std::abort();
      } catch (...) {
        LG_CRIT("capture thread crashed: unknown exception\n");
        std::abort();
      }
    });
    
    // Launch face thread only if enabled
    if (enable_face_model_) {
      LG_INFO("start_threads_after_build: launching face detection thread (RetinaFace)\n");
      face_th_ = std::thread([this]() {
        try {
          face_loop_threadfn();
        } catch (const std::exception& e) {
          LG_CRIT("face thread crashed: %s\n", e.what());
          std::abort();
        } catch (...) {
          LG_CRIT("face thread crashed: unknown exception\n");
          std::abort();
        }
      });
    }
    
    // Launch dual YOLOX worker threads only if enabled
    if (enable_yolo_model_) {
      LG_INFO("start_threads_after_build: launching YOLOX worker thread\n");
      yolo_th_ = std::thread([this]() {
        try {
          yolo_loop_threadfn();
        } catch (const std::exception& e) {
          LG_CRIT("yolo thread crashed: %s\n", e.what());
          std::abort();
        } catch (...) {
          LG_CRIT("yolo thread crashed: unknown exception\n");
          std::abort();
        }
      });
    }
  } catch (const std::exception& e) {
    LG_ERROR("start_threads_after_build: failed to launch threads: %s\n", e.what());
    return false;
  }
  
  LG_INFO("start_threads_after_build: all enabled threads launched successfully\n");
  return true;
}

void Orchestrator::stop_threads() noexcept {
  LG_INFO("stop_threads: requesting all threads to stop\n");

  // Set stop flags for all worker threads
  stop_capture_.store(true, std::memory_order_release);
  stop_face_.store(true, std::memory_order_release);
  stop_yolo_.store(true, std::memory_order_release);

  // Ask input to break capture loop
  if (input_) {
    if (auto* usb = dynamic_cast<UsbInputSource*>(input_.get())) {
      LG_INFO("stop_threads: calling input->request_stop() to unblock read()\n");
      usb->request_stop();
    }
  }

  // Give threads a grace period to exit cleanly (200ms)
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(200);

  while (std::chrono::steady_clock::now() < deadline) {
    bool all_stopped = true;
    if (capture_th_.joinable()) {
      LG_INFO("stop_threads: waiting for capture thread...\n");
      all_stopped = false;
    }
    if (face_th_.joinable()) {
      LG_INFO("stop_threads: waiting for face thread...\n");
      all_stopped = false;
    }
    if (yolo_th_.joinable()) {
      LG_INFO("stop_threads: waiting for yolo thread...\n");
      all_stopped = false;
    }
    
    if (all_stopped) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Join threads that are still running, or detach if they're stuck
  try {
    join_if(capture_th_);
    LG_INFO("stop_threads: capture thread joined\n");
  } catch (...) {
    LG_WARN("stop_threads: capture thread detached after timeout\n");
    if (capture_th_.joinable()) capture_th_.detach();
  }
  
  try {
    join_if(face_th_);
    LG_INFO("stop_threads: face thread joined\n");
  } catch (...) {
    LG_WARN("stop_threads: face thread detached after timeout\n");
    if (face_th_.joinable()) face_th_.detach();
  }
  
  try {
    join_if(yolo_th_);
    LG_INFO("stop_threads: yolo thread joined\n");
  } catch (...) {
    LG_WARN("stop_threads: yolo thread detached after timeout\n");
    if (yolo_th_.joinable()) yolo_th_.detach();
  }
  
  LG_INFO("stop_threads: all threads stopped (enable_face=%s enable_yolo=%s)\n",
          enable_face_model_ ? "true" : "false",
          enable_yolo_model_ ? "true" : "false");
}

void Orchestrator::supervisor_loop() noexcept {
  // Use configured heartbeat timeout, or default based on input type
  // USB cameras are slow (5fps = 200ms per frame), RTSP should be faster
  // Set a reasonable default: 3 seconds for USB/slow sources, allow override
  int heartbeat_timeout_ms = (cfg_.heartbeat_timeout_ms > 0) ? cfg_.heartbeat_timeout_ms : 3000;
  
  // If heartbeat_timeout_ms was set to 1000 (old hardcoded value), increase it for USB
  if (heartbeat_timeout_ms <= 1000 && 
      (!cfg_.input.usb_device.empty())) {
    heartbeat_timeout_ms = 3000;  // USB cameras need more time
    LG_INFO("supervisor_loop:detected USB source, increasing timeout to %dms\n", heartbeat_timeout_ms);
  }
  
  LG_INFO("supervisor_loop:load (heartbeat_timeout_ms=%d)\n", heartbeat_timeout_ms);
  
  // Startup grace period: Allow RTSP sources time for network wait + stream opening
  // RTSP can take 60+ seconds if network is slow to initialize
  // This prevents false "broken" detection during legitimate startup delays
  const int64_t startup_grace_period_ns = 70'000'000'000LL;  // 70 seconds
  const int64_t startup_deadline_ns = now_ns() + startup_grace_period_ns;
  
  // For recovery attempts, track when we last tried
  int64_t last_recovery_attempt_ns = 0;
  int recovery_backoff_ms = 250;
  
  while (!orchestrator_stop_.load(std::memory_order_acquire)) {
    const int64_t now = now_ns();  // Cache once per iteration, not per condition check
    const int64_t last = last_heartbeat_ns_.load(std::memory_order_relaxed);  // Use relaxed for speed
    const int64_t age_ms = (last>0) ? (now-last)/1'000'000 : 0;
    
    // Skip heartbeat checks during startup grace period to allow RTSP network wait
    const bool in_startup_grace = (now < startup_deadline_ns);
    
    if (!in_startup_grace && last && age_ms > heartbeat_timeout_ms) {
      LG_WARN("supervisor_loop:heartbeat stale (age_ms=%lld > timeout=%d)\n", age_ms, heartbeat_timeout_ms);
      source_health_.onAppsinkStarvation(now);
      source_health_.markBroken();
    }
    
    // DIAGNOSTIC: Log health state every 5 seconds (reduced from 1 second for CPU optimization)
    bool is_broken = source_health_.isBroken();
    static int64_t last_health_log_ns = 0;
    int64_t health_log_interval_ns = 5'000'000'000LL;  // Log health every 5 seconds for CPU optimization
    if ((now - last_health_log_ns) >= health_log_interval_ns) {
      LG_INFO("supervisor_loop:health (broken=%s state=%d age_ms=%lld grace=%s)\n",
              is_broken ? "true" : "false",
              static_cast<int>(state_.load(std::memory_order_acquire)),
              age_ms,
              in_startup_grace ? "active" : "expired");
      last_health_log_ns = now;
    }
    
    // Skip recovery attempts during startup grace period
    // This allows RTSP sources time to complete network wait + stream opening
    if (is_broken && !in_startup_grace) {
      state_.store(OrchestratorState::Recovering, std::memory_order_release);
      
      // Implement adaptive retry: keep trying with exponential backoff
      const int64_t time_since_last_attempt_ms = 
          (last_recovery_attempt_ns > 0) ? (now - last_recovery_attempt_ns) / 1'000'000 : recovery_backoff_ms + 1;
      
      LG_INFO("supervisor_loop:broken state check (elapsed_since_attempt=%lldms, backoff_needed=%dms)\n",
              time_since_last_attempt_ms, recovery_backoff_ms);
      
      if (time_since_last_attempt_ms >= recovery_backoff_ms) {
        LG_INFO("supervisor_loop:recover_pipeline (reason: health broken, attempt after %lldms backoff)\n", time_since_last_attempt_ms);
        last_recovery_attempt_ns = now;
        
        if (!recover_pipeline(now)) {
          // Recovery failed, increase backoff exponentially
          recovery_backoff_ms = static_cast<int>(std::min(8000LL, static_cast<long long>(recovery_backoff_ms) * 2));
          LG_WARN("supervisor_loop:recovery attempt failed, next retry in %dms\n", recovery_backoff_ms);
        } else {
          // Recovery succeeded: reset backoff and update timestamp to NOW (recovery end),
          // not the pre-recovery 'now'. This ensures the post-recovery grace period is
          // enforced from when the new pipeline actually started, not from when we began
          // tearing down the old one. Without this, a 2-3s recovery means time_since_last
          // is already >> backoff_ms the moment recover_pipeline returns, causing an
          // immediate re-recovery if health flickers broken again during thread startup.
          last_recovery_attempt_ns = now_ns();
          recovery_backoff_ms = 2000;  // 2s grace before allowing another recovery attempt
          LG_INFO("supervisor_loop:recovery succeeded, next attempt allowed in %dms\n", recovery_backoff_ms);
        }
      } else {
        LG_INFO("supervisor_loop:waiting for backoff (elapsed=%lldms, need=%dms)\n", 
                time_since_last_attempt_ms, recovery_backoff_ms);
      }
    } else if (state_.load(std::memory_order_acquire) == OrchestratorState::Recovering) {
      LG_INFO("supervisor_loop:health recovered, transitioning from Recovering to Running\n");
      state_.store(OrchestratorState::Running, std::memory_order_release);
      recovery_backoff_ms = 250;  // Reset backoff when healthy
      last_recovery_attempt_ns = 0;
    }
    
    // Publish analytics results periodically (every second)
    static int64_t last_publish_ns = 0;
    static uint64_t last_publish_seq = 0;
    const int64_t publish_interval_ns = 1'000'000'000LL;  // 1 second
    if ((now - last_publish_ns) >= publish_interval_ns && !publishers_.empty()) {
      const int64_t elapsed_ns = (last_publish_ns == 0) ? publish_interval_ns : (now - last_publish_ns);
      const uint64_t current_seq = frame_seq_.load(std::memory_order_relaxed);
      const uint64_t frames_processed = (last_publish_seq == 0) ? 0 : (current_seq - last_publish_seq);
      
      // Calculate actual FPS from frame processing
      const int calculated_fps = (elapsed_ns > 0 && frames_processed > 0) 
                                 ? static_cast<int>((frames_processed * 1'000'000'000LL) / elapsed_ns)
                                 : 0;
      
      last_publish_ns = now;
      last_publish_seq = current_seq;
      
      // Build PipelineResult from fusion state
      PipelineResult result{};
      std::vector<Detection> yolo_dets_copy;  // Copy for tracker update
      std::vector<TrackedBox> tracks;  // Tracking results
      
      // CRITICAL: Declare snapshot variables outside lock scope for use throughout iteration
      // This prevents race condition where faces are cleared between global count and matching
      std::vector<Detection> face_dets_snapshot;
      std::vector<Landmarks> face_lms_snapshot;
      
      {
        std::lock_guard<std::mutex> lk(fusion_.m);
        
        // Copy YOLOX detections for tracker
        yolo_dets_copy = fusion_.yolo_dets;
        
        // Count YOLOX person detections (class_id == 0 in COCO dataset)
        // Also check confidence score to filter out low-confidence detections
        // De-letterbox params to convert face dets (model/320x320) -> camera space
        const float dc_model = 320.0f;
        const int dc_cam_w = fusion_.frame_width  > 0 ? fusion_.frame_width  : 640;
        const int dc_cam_h = fusion_.frame_height > 0 ? fusion_.frame_height : 480;
        const float dc_s     = std::min(dc_model / dc_cam_w, dc_model / dc_cam_h);
        const float dc_pad_x = (dc_model - dc_cam_w * dc_s) / 2.0f;
        const float dc_pad_y = (dc_model - dc_cam_h * dc_s) / 2.0f;
        // Require face center inside person bbox to reject hand/limb false positives.
        // Falls back (accepts all) when no face detections are present (cold start).
        auto person_has_face = [&](const Detection& pd) -> bool {
          if (fusion_.face_dets.empty()) return true;
          for (const auto& fd : fusion_.face_dets) {
            const float fcx = ((fd.x0 + fd.x1) * 0.5f - dc_pad_x) / dc_s;
            const float fcy = ((fd.y0 + fd.y1) * 0.5f - dc_pad_y) / dc_s;
            if (fcx >= pd.x0 && fcx <= pd.x1 && fcy >= pd.y0 && fcy <= pd.y1) return true;
          }
          return false;
        };
        int person_count = 0;
        const float min_confidence = 0.5f;  // Minimum confidence threshold
        const float count_frame_h = static_cast<float>(fusion_.frame_height > 0 ? fusion_.frame_height : 480);
        for (const auto& det : fusion_.yolo_dets) {
          if (det.class_id != 0 || det.score < min_confidence) continue;
          const float dh = det.y1 - det.y0;
          if (dh < 0.12f * count_frame_h) continue;
          if (!person_has_face(det)) continue;  // reject hands/limbs without a face
          person_count++;
        }
        result.people_count = person_count;
        
        // Copy face detections for consistent snapshot
        // CRITICAL FIX: Convert faces from model/letterbox space (320x320) to camera space (e.g., 1920x1080)
        // This ensures IoU matching works correctly (both faces and tracks in same coordinate system)
        face_dets_snapshot.clear();
        face_lms_snapshot.clear();
        
        // Calculate de-letterbox transform (same as visualization.cpp)
        const float model_size = 320.0f;
        const int cam_w = fusion_.frame_width;   // e.g., 1920
        const int cam_h = fusion_.frame_height;  // e.g., 1080
        const float s = std::min(model_size / cam_w, model_size / cam_h);  // letterbox scale
        const float pad_x = (model_size - cam_w * s) / 2.0f;
        const float pad_y = (model_size - cam_h * s) / 2.0f;
        
        static int delbox_log_count = 0;
        if (delbox_log_count < 3) {
          LG_INFO("[COORD-FIX] De-letterbox transform: cam=%dx%d model=320x320 s=%.3f pad=(%.1f,%.1f)", 
                  cam_w, cam_h, s, pad_x, pad_y);
          delbox_log_count++;
        }
        
        // Transform each face AND its landmarks from model/letterbox space → camera space
        for (size_t i = 0; i < fusion_.face_dets.size(); ++i) {
          const auto& face_model = fusion_.face_dets[i];

          // Landmark geometry gate (model space): reject hands/objects before accepting
          // detection into analytics or gaze tracking.
          if (i < fusion_.face_lms.size()) {
            const auto& lms_m = fusion_.face_lms[i];
            // Inline the same checks as visualization::is_plausible_face()
            const float le_y  = lms_m.pts[1], re_y = lms_m.pts[3];
            const float n_y   = lms_m.pts[5];
            const float lmy   = lms_m.pts[7], rmy = lms_m.pts[9];
            const float eye_mid_y   = (le_y + re_y) * 0.5f;
            const float mouth_mid_y = (lmy + rmy) * 0.5f;
            const float dx = lms_m.pts[2] - lms_m.pts[0];
            const float dy = re_y - le_y;
            const float iod    = std::sqrt(dx*dx + dy*dy);
            const float face_w = face_model.x1 - face_model.x0;
            const float face_h = face_model.y1 - face_model.y0;
            const bool valid = (face_w > 0.f && face_h > 0.f)
                             && (n_y > eye_mid_y + 1.0f)
                             && (mouth_mid_y > n_y + 1.0f)
                             && (iod > 0.10f * face_w) && (iod < 0.95f * face_w)
                             && (face_h / face_w > 0.55f);
            if (!valid) {
              static int orch_reject_log = 0;
              if (++orch_reject_log <= 10 || orch_reject_log % 60 == 0)
                LG_INFO("[ORCH-FACE-FILTER] Rejected face#%zu as non-face (landmark geometry fail)", i);
              continue;
            }
          }

          Detection face_cam = face_model;  // Copy
          
          // De-letterbox face bbox: (model coords - padding) / scale = camera coords
          face_cam.x0 = (face_model.x0 - pad_x) / s;
          face_cam.y0 = (face_model.y0 - pad_y) / s;
          face_cam.x1 = (face_model.x1 - pad_x) / s;
          face_cam.y1 = (face_model.y1 - pad_y) / s;
          
          face_dets_snapshot.push_back(face_cam);
          
          // CRITICAL: Also transform landmarks to camera space for gaze detection
          if (i < fusion_.face_lms.size()) {
            const auto& lms_model = fusion_.face_lms[i];
            Landmarks lms_cam = lms_model;  // Copy
            
            // Transform each of the 5 landmark points (left_eye, right_eye, nose, left_mouth, right_mouth)
            for (int j = 0; j < 5; ++j) {
              lms_cam.pts[j * 2 + 0] = (lms_model.pts[j * 2 + 0] - pad_x) / s;  // x
              lms_cam.pts[j * 2 + 1] = (lms_model.pts[j * 2 + 1] - pad_y) / s;  // y
            }
            
            face_lms_snapshot.push_back(lms_cam);
          }
          
          // Debug: Log first few transformations
          if (delbox_log_count < 3 && face_dets_snapshot.size() <= 2) {
            LG_INFO("[COORD-FIX] Face #%zu: model=(%.1f,%.1f,%.1f,%.1f) → camera=(%.1f,%.1f,%.1f,%.1f)",
                    face_dets_snapshot.size() - 1,
                    face_model.x0, face_model.y0, face_model.x1, face_model.y1,
                    face_cam.x0, face_cam.y0, face_cam.x1, face_cam.y1);
            if (i < fusion_.face_lms.size()) {
              const auto& lms_model = fusion_.face_lms[i];
              const auto& lms_cam = face_lms_snapshot.back();
              LG_INFO("[COORD-FIX] Landmarks #%zu: left_eye model=(%.1f,%.1f) → camera=(%.1f,%.1f)",
                      i, lms_model.pts[0], lms_model.pts[1], lms_cam.pts[0], lms_cam.pts[1]);
            }
          }
        }
        
        // Count RetinaFace face detections from snapshot (all faces are gaze candidates)
        // RetinaFace detections already have confidence filtering applied
        result.gaze_count = static_cast<int>(face_dets_snapshot.size());
        
        // Debug: Log gaze count at analytics time
        static int analytics_debug_counter = 0;
        if (++analytics_debug_counter % 3 == 0) {
          LG_INFO("[ANALYTICS] Global gaze_count=%d (face_dets_snapshot.size=%zu)", 
                  result.gaze_count, face_dets_snapshot.size());
        }
        
        result.ts_ns = static_cast<uint64_t>(now);
        result.seq = current_seq;
        result.fps = calculated_fps;
        result.frame_width = fusion_.frame_width;   // V6.2: For normalized speed
        result.frame_height = fusion_.frame_height; // V6.2: For normalized speed
      }
      
      // Update person tracker with YOLOX detections (outside lock)
      {
        // Filter person detections for tracker with enhanced thresholds
        std::vector<Detection> people;
        people.reserve(yolo_dets_copy.size());
        for (const auto& det : yolo_dets_copy) {
          // Apply stricter filtering: class, score, and area
          if (det.class_id != 0) continue;  // Person only
          if (det.score < 0.35f) continue;  // Lowered to catch partially-visible employees
          
          // Calculate bbox area
          int w = static_cast<int>(det.x1 - det.x0);
          int h = static_cast<int>(det.y1 - det.y0);
          int area = w * h;
          if (area < 1600) continue;  // ~40x40 minimum

          // Reject limb/hand false positives: must be at least 12% of frame height.
          const float frame_h_f = static_cast<float>(fusion_.frame_height > 0 ? fusion_.frame_height : 480);
          if (static_cast<float>(h) < 0.12f * frame_h_f) continue;

          // Reject if no face center lies inside this person bbox (face_dets_snapshot is
          // already in camera space, built just above in the same iteration).
          if (!face_dets_snapshot.empty()) {
            bool found = false;
            for (const auto& fd : face_dets_snapshot) {
              const float fcx = (fd.x0 + fd.x1) * 0.5f;
              const float fcy = (fd.y0 + fd.y1) * 0.5f;
              if (fcx >= det.x0 && fcx <= det.x1 && fcy >= det.y0 && fcy <= det.y1) {
                found = true; break;
              }
            }
            if (!found) continue;
          }

          people.push_back(det);
        }
        
        // Simple NMS to remove duplicate detections (IoU > 0.5)
        // Sort by score descending
        std::sort(people.begin(), people.end(), 
                  [](const Detection& a, const Detection& b) { return a.score > b.score; });
        
        std::vector<Detection> nms_filtered;
        nms_filtered.reserve(people.size());
        std::vector<bool> suppressed(people.size(), false);
        
        for (size_t i = 0; i < people.size(); ++i) {
          if (suppressed[i]) continue;
          nms_filtered.push_back(people[i]);
          
          // Suppress overlapping boxes
          for (size_t j = i + 1; j < people.size(); ++j) {
            if (suppressed[j]) continue;
            
            // Calculate IoU
            float x0 = std::max(people[i].x0, people[j].x0);
            float y0 = std::max(people[i].y0, people[j].y0);
            float x1 = std::min(people[i].x1, people[j].x1);
            float y1 = std::min(people[i].y1, people[j].y1);
            float inter_w = std::max(0.f, x1 - x0);
            float inter_h = std::max(0.f, y1 - y0);
            float inter = inter_w * inter_h;
            
            float area_i = (people[i].x1 - people[i].x0) * (people[i].y1 - people[i].y0);
            float area_j = (people[j].x1 - people[j].x0) * (people[j].y1 - people[j].y0);
            float uni = area_i + area_j - inter;
            float iou = (uni > 0) ? (inter / uni) : 0.f;
            
            if (iou > 0.5f) {
              suppressed[j] = true;
            }
          }
        }
        
        // Update tracker with filtered detections
        const double ts_s = now * 1e-9;  // Convert nanoseconds to seconds
        tracks = person_tracker_.update(nms_filtered, ts_s);
        
        // Associate face detections with person tracks for gaze data
        // Use snapshot captured earlier to ensure consistency with global count
        
        // Debug: Log face detection count and matching
        static int debug_gaze_counter = 0;
        debug_gaze_counter++;
        if (debug_gaze_counter % 3 == 0) {  // Log every 3 seconds for debugging
          LG_INFO("[GAZE] Face detections: %zu, Person tracks: %zu (using snapshot)", 
                  face_dets_snapshot.size(), tracks.size());
          // Log face bounding boxes when faces are detected
          for (size_t i = 0; i < face_dets_snapshot.size(); ++i) {
            const auto& face = face_dets_snapshot[i];
            LG_INFO("[GAZE-FACE] Face %zu: bbox=[%.1f,%.1f,%.1f,%.1f] score=%.2f", 
                    i, face.x0, face.y0, face.x1, face.y1, face.score);
          }
        }
        
        // For each person track, find best matching face detection
        for (auto& track : tracks) {
          // Reset gaze info for this frame
          track.has_gaze = false;
          track.is_gazing = false;
          
          // Debug: Log track bbox when we have faces to match
          bool should_log = (debug_gaze_counter % 3 == 0) && (face_dets_snapshot.size() > 0);
          if (should_log) {
            LG_INFO("[GAZE-TRACK] Track %d: bbox=[%.1f,%.1f,%.1f,%.1f]", 
                    track.id, track.x0, track.y0, track.x1, track.y1);
          }
          
          // Find best overlapping face detection
          float best_iou = 0.0f;
          int best_face_idx = -1;
          
          // Expand person bbox upward to include head region (faces detected above person bbox)
          // Observed: faces at y=120-180, person boxes start at y=280+ (YOLOX crops heads)
          float track_height = track.y1 - track.y0;
          float head_expansion = track_height * 0.3f;  // Expand 30% of body height upward for head
          float expanded_y0 = std::max(0.f, track.y0 - head_expansion);
          
          for (size_t i = 0; i < face_dets_snapshot.size(); ++i) {
            const auto& face = face_dets_snapshot[i];
            
            // Calculate IoU between EXPANDED person bbox and face bbox
            float x0 = std::max(track.x0, face.x0);
            float y0 = std::max(expanded_y0, face.y0);  // Use expanded top edge
            float x1 = std::min(track.x1, face.x1);
            float y1 = std::min(track.y1, face.y1);
            float inter_w = std::max(0.f, x1 - x0);
            float inter_h = std::max(0.f, y1 - y0);
            float inter = inter_w * inter_h;
            
            float face_area = (face.x1 - face.x0) * (face.y1 - face.y0);
            float expanded_track_area = (track.x1 - track.x0) * (track.y1 - expanded_y0);  // Use expanded height
            float uni = face_area + expanded_track_area - inter;
            float iou = (uni > 0) ? (inter / uni) : 0.f;
            
            // Also check if face center is within EXPANDED person bbox (handles face smaller than body)
            float face_cx = (face.x0 + face.x1) * 0.5f;
            float face_cy = (face.y0 + face.y1) * 0.5f;
            bool face_inside_track = (face_cx >= track.x0 && face_cx <= track.x1 &&
                                     face_cy >= expanded_y0 && face_cy <= track.y1);  // Use expanded top
            
            // Accept if good IoU OR face center is inside person bbox
            // LOWERED from 0.1 to 0.05: faces are small (66-96px) compared to full body boxes
            if ((iou > 0.05f || face_inside_track) && iou > best_iou) {
              best_iou = iou;
              best_face_idx = static_cast<int>(i);
            }
            
            // Debug: Log IoU calculations when we have faces
            if (should_log) {
              LG_INFO("[GAZE-IOU] Track %d <-> Face %zu: IoU=%.3f, face_inside=%d, accepted=%d", 
                      track.id, i, iou, face_inside_track ? 1 : 0, 
                      (iou > 0.05f || face_inside_track) ? 1 : 0);
            }
          }
          
          // If we found a matching face, check gaze and update track
          if (best_face_idx >= 0) {
            const auto& face = face_dets_snapshot[best_face_idx];
            track.has_gaze = true;
            track.face_bbox_x0 = face.x0;
            track.face_bbox_y0 = face.y0;
            track.face_bbox_x1 = face.x1;
            track.face_bbox_y1 = face.y1;
            
            // DEBUG: Always log when we set has_gaze=true
            LG_INFO("[GAZE-MATCH] Track %d MATCHED to Face %d: IoU=%.3f, face_bbox=[%.1f,%.1f,%.1f,%.1f]", 
                    track.id, best_face_idx, best_iou, face.x0, face.y0, face.x1, face.y1);
            
            // Convert Detection to retinaface_object_t for gaze check
            if (best_face_idx < static_cast<int>(face_lms_snapshot.size())) {
              retinaface_object_t rf_face{};
              rf_face.box.left = static_cast<int>(face.x0);
              rf_face.box.top = static_cast<int>(face.y0);
              rf_face.box.right = static_cast<int>(face.x1);
              rf_face.box.bottom = static_cast<int>(face.y1);
              rf_face.score = face.score;
              
              // Copy landmarks if available
              const auto& lms = face_lms_snapshot[best_face_idx];
              for (int j = 0; j < 5; ++j) {
                rf_face.ponit[j].x = static_cast<int>(lms.pts[j * 2 + 0]);
                rf_face.ponit[j].y = static_cast<int>(lms.pts[j * 2 + 1]);
              }
              
              // Check if face is looking at camera
              track.is_gazing = face_is_looking_at_us(rf_face);
              
              // Update gaze time using persistent map (increment by publish interval ~1 second)
              if (track.is_gazing) {
                gaze_time_map_[track.id] += 1.0;  // Add 1 second of gaze time
              }
              track.gaze_time = gaze_time_map_[track.id];  // Apply accumulated time
              
              // DEBUG: Always log gaze details
              LG_INFO("[GAZE-DIR] Track %d: is_gazing=%d, gaze_time=%.1f (total accumulated)", 
                      track.id, track.is_gazing ? 1 : 0, track.gaze_time);
              
              // DEBUG: Always log gaze details
              LG_INFO("[GAZE-DIR] Track %d: is_gazing=%d, gaze_time=%.1f (total accumulated)", 
                      track.id, track.is_gazing ? 1 : 0, track.gaze_time);
              
              // Debug: Log every gaze match
              if (debug_gaze_counter % 3 == 0) {
                LG_INFO("[GAZE] Track %d: matched face (IoU=%.2f), gazing=%d, time=%.1f", 
                        track.id, best_iou, track.is_gazing ? 1 : 0, track.gaze_time);
              }
            }
          } else {
            // DEBUG: Log when track has no matching face
            if (should_log) {
              LG_INFO("[GAZE-NOMATCH] Track %d: no face match (best_iou=%.3f, faces=%zu)", 
                      track.id, best_iou, face_dets_snapshot.size());
            }
          }
        }  // End for (auto& track : tracks)

        // DEBUG: Log matching summary
        int tracks_with_gaze_match = 0;
        int tracks_gazing = 0;
        for (const auto& t : tracks) {
          if (t.has_gaze) tracks_with_gaze_match++;
          if (t.has_gaze && t.is_gazing) tracks_gazing++;
        }
        if (debug_gaze_counter % 3 == 0 && face_dets_snapshot.size() > 0) {
          LG_INFO("[GAZE-SUMMARY] Faces detected: %zu, Tracks: %zu, Matched: %d, Gazing: %d",
                  face_dets_snapshot.size(), tracks.size(), tracks_with_gaze_match, tracks_gazing);
        }
        
        // Emission cache: hold last non-empty tracks for brief detector misses
        constexpr double HOLD_TTL_S = 0.5;  // 500ms hold time
        
        std::vector<TrackedBox> tracks_to_emit;
        bool is_stale = false;
        
        if (!tracks.empty()) {
          // Fresh tracks available
          tracks_to_emit = tracks;
          emit_cache_.last_nonempty = tracks;
          emit_cache_.last_nonempty_ts = ts_s;
          
          // Cleanup gaze_time_map: remove entries for tracks that no longer exist
          std::unordered_set<int> active_ids;
          for (const auto& t : tracks) {
            active_ids.insert(t.id);
          }
          for (auto it = gaze_time_map_.begin(); it != gaze_time_map_.end(); ) {
            if (active_ids.find(it->first) == active_ids.end()) {
              it = gaze_time_map_.erase(it);
            } else {
              ++it;
            }
          }
        } else if ((ts_s - emit_cache_.last_nonempty_ts) <= HOLD_TTL_S) {
          // No fresh tracks, but within hold period - reuse last.
          tracks_to_emit = emit_cache_.last_nonempty;
          is_stale = true;
        } else {
          // Beyond hold period - emit empty
          tracks_to_emit.clear();
        }
        
        // Store tracks in fusion state for other consumers
        {
          std::lock_guard<std::mutex> lk(fusion_.m);
          fusion_.tracks = tracks_to_emit;
        }
        
        // Add tracks to PipelineResult for publishers
        result.person_tracks = tracks_to_emit;
        
        // Count tracks that actually have gaze data (faces matched to people)
        int tracks_with_gaze = 0;
        for (const auto& t : tracks_to_emit) {
          if (t.has_gaze) {
            tracks_with_gaze++;
          }
        }
        result.gaze_count = tracks_with_gaze;  // Override: count matched faces, not total detections
        
        // DEBUG: Log track states before publishing
        static int pre_publish_log_counter = 0;
        if (++pre_publish_log_counter % 3 == 0 && !tracks_to_emit.empty()) {
          LG_INFO("[PRE-PUBLISH] Sending %zu tracks to MQTT (gaze_count=%d):", 
                  tracks_to_emit.size(), tracks_with_gaze);
          for (const auto& t : tracks_to_emit) {
            LG_INFO("  Track %d: has_gaze=%d, is_gazing=%d, gaze_time=%.1f", 
                    t.id, t.has_gaze ? 1 : 0, t.is_gazing ? 1 : 0, t.gaze_time);
          }
        }
        
        // Override people_count to match emitted tracks (not raw detections)
        result.people_count = static_cast<int>(tracks_to_emit.size());
        
        // TODO: Could add stale flag to PipelineResult if needed for telemetry
        // result.tracks_stale = is_stale;
      }
      
      // Publish to all publishers
      for (auto& pub : publishers_) {
        if (pub) {
          pub->publish_result(result);
        }
      }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 10 Hz MQTT publishing (was 200ms/5Hz)
  }
}

void Orchestrator::mark_broken(FaultCode, int64_t now) noexcept {
  source_health_.onNoFrames(now);
  source_health_.markBroken();
}

bool Orchestrator::recover_pipeline(int64_t now_ns_val) noexcept {
    LG_INFO("recover_pipeline:starting recovery attempt\n");

    // 1. Stop (or quarantine) any existing worker
    LG_INFO("recover_pipeline:stopping worker\n");
    stop_threads();  // bounded, returns fast

    // We are STILL considered broken until we actually launch a new worker.
    // DO NOT clearBroken() yet.
    // DO NOT update last_heartbeat_ns_ yet.
    // DO NOT set state_ = Running yet.

    // 2. Figure out what device we *should* try to use.
    //
    // If the pipeline was originally configured with an explicit network/file
    // source (RTSP/HTTP/file, e.g. from argus-config.json), we must reconnect to
    // THAT source. Only USB/registry-driven camera setups should consult the
    // registry video-device on recovery. Previously we always queried the
    // registry, so an RTSP input whose stream briefly dropped would be
    // "recovered" as the registry's USB node (e.g. /dev/video1) and could never
    // reconnect - the supervisor then looped forever scanning for a USB camera
    // that does not exist.
    const SourceKind orig_kind = detect_source_kind(original_input_);
    if (orig_kind == SourceKind::RTSP || orig_kind == SourceKind::File) {
        // Restore the pristine original input and reconnect to it directly.
        cfg_.input = original_input_;
        const std::string& url = !original_input_.rtsp_url.empty()
                                     ? original_input_.rtsp_url
                                     : original_input_.file_path;
        LG_INFO("recover_pipeline:reconnecting original %s source %s\n",
                orig_kind == SourceKind::RTSP ? "RTSP" : "file", url.c_str());
        // Fall through to the availability check + rebuild below (steps 4-7).
    } else {
    //    For USB/registry-driven sources we consult the registry on every
    //    attempt so we can hop e.g. /dev/video1 -> /dev/video2.
    const std::string reg_choice = RegistryHelper::getVideoDevice(); 
    // reg_choice is "usb_camera", or "/dev/video1", or "rtsp://...", etc.

    // We'll try to resolve a live device node:
    std::string candidate_dev;

    if (reg_choice.empty() || reg_choice == "usb_camera") {
        // Dynamic USB camera mode.
        // Always rescan, EVERY attempt, so we can hop from /dev/video1 to /dev/video2.
        std::string scanned = RegistryHelper::findWorkingCameraDevice();
        if (!scanned.empty()) {
            LG_INFO("recover_pipeline:scan found working USB camera at %s\n", scanned.c_str());
            candidate_dev = scanned;
        } else {
            LG_INFO("recover_pipeline:no USB camera found yet (usb_camera mode)\n");
        }
    } else if (reg_choice.rfind("/dev/video", 0) == 0) {
        // User pinned e.g. "/dev/video1"
        candidate_dev = reg_choice;

        // If that node vanished, still fall back to scanning.
        struct stat st;
        if (stat(candidate_dev.c_str(), &st) != 0) {
            LG_WARN("recover_pipeline:requested node %s missing, scanning alternatives\n",
                    candidate_dev.c_str());
            std::string scanned = RegistryHelper::findWorkingCameraDevice();
            if (!scanned.empty()) {
                LG_INFO("recover_pipeline:using fallback USB camera %s\n", scanned.c_str());
                candidate_dev = scanned;
            } else {
                LG_INFO("recover_pipeline:no alternate USB camera available\n");
            }
        }
    } else if (reg_choice.rfind("rtsp://", 0) == 0) {
        // RTSP URL is directly usable
        candidate_dev = reg_choice;
        LG_INFO("recover_pipeline:using RTSP stream %s\n", reg_choice.c_str());
    } else if (reg_choice.rfind("http://", 0) == 0 || reg_choice.rfind("https://", 0) == 0) {
        // HTTP stream is also usable
        candidate_dev = reg_choice;
        LG_INFO("recover_pipeline:using HTTP stream %s\n", reg_choice.c_str());
    } else {
        // Could be file mode, or garbage.
        LG_WARN("recover_pipeline:unrecognized reg_choice '%s'\n", reg_choice.c_str());
        candidate_dev.clear();
    }

    // 3. Handle both device nodes and stream URLs
    if (!candidate_dev.empty()) {
        if (candidate_dev.rfind("/dev/video", 0) == 0) {
            // USB device path
            cfg_.input.usb_device = candidate_dev;
            cfg_.input.rtsp_url.clear();
        } else if (candidate_dev.rfind("rtsp://", 0) == 0 || 
                   candidate_dev.rfind("http://", 0) == 0 ||
                   candidate_dev.rfind("https://", 0) == 0) {
            // Network stream URL
            cfg_.input.rtsp_url = candidate_dev;
            cfg_.input.usb_device.clear();
        }
    }
    }  // end else: USB/registry-driven source resolution

    // 4. Is that device/stream actually accessible *right now*?
    bool device_available = false;
    std::string device_desc;
    
    if (!cfg_.input.usb_device.empty()) {
        struct stat st;
        if (stat(cfg_.input.usb_device.c_str(), &st) == 0) {
            device_available = true;
        }
        device_desc = cfg_.input.usb_device;
        LG_INFO("recover_pipeline:USB device %s %s\n",
                cfg_.input.usb_device.c_str(),
                device_available ? "available" : "not yet available");
    } else if (!cfg_.input.rtsp_url.empty()) {
        // For RTSP URLs, assume available (actual connection happens in open())
        device_available = true;
        device_desc = cfg_.input.rtsp_url;
        LG_INFO("recover_pipeline:RTSP stream %s available for connection\n",
                cfg_.input.rtsp_url.c_str());
    } else if (!cfg_.input.file_path.empty()) {
        // For file sources, stat() the path so a missing file is retried later
        // instead of driving futile rebuild/open cycles (and log spam).
        struct stat st;
        if (stat(cfg_.input.file_path.c_str(), &st) == 0) {
            device_available = true;
        }
        device_desc = cfg_.input.file_path;
        LG_INFO("recover_pipeline:file source %s %s\n",
                cfg_.input.file_path.c_str(),
                device_available ? "available" : "missing");
    } else {
        LG_INFO("recover_pipeline:no device or stream configured, nothing to open yet\n");
    }

    if (!device_available) {
        LG_INFO("recover_pipeline:device/stream not yet available, will retry later\n");
        // We are still broken. Supervisor will call us again.
        return false;
    }

    // 5. Build brand new input source / runner using that device.
    LG_INFO("recover_pipeline:rebuilding pipeline around %s\n", device_desc.c_str());

    std::unique_ptr<IInputSource> new_input_tmp;
    try { new_input_tmp = make_input(cfg_.input); } catch (...) { new_input_tmp.reset(); }
    if (!new_input_tmp) {
        LG_ERROR("recover_pipeline:failed to create input\n");
        return false; // still broken
    }

    if (!new_input_tmp->open()) {
        LG_ERROR("recover_pipeline:input->open() failed for %s\n", device_desc.c_str());
        return false; // still broken
    }

    if (!new_input_tmp->start()) {
        LG_ERROR("recover_pipeline:input->start() failed\n");
        new_input_tmp->close();
        return false; // still broken
    }

    // Create face runner only if enabled
    std::shared_ptr<IModelRunner> new_face_runner;
    if (enable_face_model_) {
        std::unique_ptr<IModelRunner> new_face_runner_tmp = make_model_runner(cfg_.primary_model);
        if (!new_face_runner_tmp) {
            LG_ERROR("recover_pipeline:failed to create face runner\n");
            new_input_tmp->stop();
            new_input_tmp->close();
            return false; // still broken
        }

        if (!new_face_runner_tmp->load(cfg_.primary_model)) {
            LG_ERROR("recover_pipeline:face model load failed: %s\n",
                     cfg_.primary_model.model_path.c_str());
            new_input_tmp->stop();
            new_input_tmp->close();
            return false; // still broken
        }
        new_face_runner = std::shared_ptr<IModelRunner>(std::move(new_face_runner_tmp));
    }

    // Create yolo runner only if enabled
    std::shared_ptr<IModelRunner> new_yolo_runner;
    if (enable_yolo_model_) {
        std::unique_ptr<IModelRunner> new_yolo_runner_tmp = make_model_runner(cfg_.secondary_model);
        if (!new_yolo_runner_tmp) {
            LG_ERROR("recover_pipeline:failed to create yolo runner\n");
            new_input_tmp->stop();
            new_input_tmp->close();
            return false; // still broken
        }

        if (!new_yolo_runner_tmp->load(cfg_.secondary_model)) {
            LG_ERROR("recover_pipeline:yolo model load failed: %s\n",
                     cfg_.secondary_model.model_path.c_str());
            new_input_tmp->stop();
            new_input_tmp->close();
            return false; // still broken
        }
        new_yolo_runner = std::shared_ptr<IModelRunner>(std::move(new_yolo_runner_tmp));
    }

    // 6. Publish new pipeline objects
    auto new_input = std::shared_ptr<IInputSource>(std::move(new_input_tmp));
    
    input_  = new_input;
    face_runner_ = new_face_runner;
    yolo_runner_ = new_yolo_runner;

    // 7. Reset orchestrator run state for NEW worker threads
    LG_INFO("recover_pipeline:resetting run state for new worker threads\n");
    stop_capture_.store(false, std::memory_order_release);
    stop_face_.store(false, std::memory_order_release);
    stop_yolo_.store(false, std::memory_order_release);

    LG_INFO("recover_pipeline:launching new worker threads (enable_face=%s enable_yolo=%s)\n",
            enable_face_model_ ? "true" : "false",
            enable_yolo_model_ ? "true" : "false");
    try {
        capture_th_ = std::thread(&Orchestrator::capture_loop_threadfn, this);
        
        // Launch face thread only if enabled
        if (enable_face_model_) {
            face_th_    = std::thread(&Orchestrator::face_loop_threadfn, this);
        }
        
        // Launch YOLOX worker thread only if enabled
        if (enable_yolo_model_) {
            yolo_th_ = std::thread(&Orchestrator::yolo_loop_threadfn, this);
        }
    } catch (const std::exception& e) {
        LG_ERROR("recover_pipeline:failed to launch threads: %s\n", e.what());
        return false;
    }

    // 8. Mark healthy again only NOW.
    source_health_.clearBroken();
    last_heartbeat_ns_.store(now_ns(), std::memory_order_release);
    state_.store(OrchestratorState::Running, std::memory_order_release);

    // Reset person tracker after successful recovery
    person_tracker_.reset();
    LG_INFO("recover_pipeline:tracker reset\n");

    LG_INFO("recover_pipeline:pipeline restored on %s, running again\n",
            device_desc.c_str());
    return true;
}
void Orchestrator::capture_loop_threadfn() noexcept {
    LG_INFO("capture_loop:start");

    while (!stop_capture_.load(std::memory_order_relaxed)) {
        // Try to fetch next frame from input source
        FrameView camView{};
        FetchStatus st = input_->tryFetch(camView);

        if (st != FetchStatus::Ok) {
            // Camera hiccup or timeout
            source_health_.onBusError(now_ns(), "capture fetch fail");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Healthy frame received
        source_health_.onFrameOk(camView.pts_ns);

        // Wrap into SharedFrame (zero-copy, just wrap the buffer)
        auto sf = std::make_shared<SharedFrame>();
        sf->pts_ns = camView.pts_ns;
        sf->width  = camView.width;   // Preprocessed dimensions (e.g., 320×320)
        sf->height = camView.height;
        sf->seq    = frame_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        
        // V7.1: Preserve original camera/stream dimensions for coordinate transforms
        // RTSP: orig=1920×1080, preprocessed=320×320
        // USB:  orig=640×480, preprocessed=320×320 (or whatever USB provides)
        sf->orig_width  = camView.orig_width;
        sf->orig_height = camView.orig_height;
        
        // V7.1: Debug - verify dimensions are propagating correctly
        static int sf_debug_count = 0;
        if (sf_debug_count < 3) {
            LG_INFO("[SF-COPY] camView: w=%d h=%d orig_w=%d orig_h=%d -> sf: w=%d h=%d orig_w=%d orig_h=%d",
                    camView.width, camView.height, camView.orig_width, camView.orig_height,
                    sf->width, sf->height, sf->orig_width, sf->orig_height);
            sf_debug_count++;
        }

        // Copy BGR data (this is the one place we duplicate pixel data)
        sf->bgr.resize(static_cast<size_t>(sf->width * sf->height * 3));
        if (camView.plane0) {
            std::memcpy(sf->bgr.data(), camView.plane0, sf->bgr.size());
        }

        // Fan out to enabled model mailboxes (lock-free post)
        // Only post to mailboxes that have active consumers
        if (enable_face_model_) {
            mb_face_.postFrame(sf);
        }

        // Fan out to enabled model mailboxes
        if (enable_yolo_model_) {
            mb_yolo_.postFrame(sf);
        }

        // Update heartbeat for supervisor
        last_heartbeat_ns_.store(now_ns(), std::memory_order_release);
    }

    LG_INFO("capture_loop:stop");
}

void Orchestrator::face_loop_threadfn() noexcept {
    try {
        LG_INFO("face_loop:start (RetinaFace on NPU core 0)");

        // Guard: check if face model is enabled
        if (!enable_face_model_) {
            LG_WARN("face_loop: face model disabled, exiting thread");
            return;
        }

        if (!face_runner_) {
            LG_ERROR("face_loop: face_runner_ not initialized but enable_face_model_=true");
            return;
        }

        // Configure generic worker for face detection
        inference_worker::WorkerConfig config{};
        config.skip_frames = cfg_.face_skip_frames;  // 0/1 = every frame (from config)
        config.model_input_width = face_runner_->spec().input_size.w;
        config.model_input_height = face_runner_->spec().input_size.h;
        config.model_name = "RetinaFace";
        config.flip_horizontal = cfg_.flip_horizontal;  // Mirror correction
        config.log_performance = cfg_.log_performance;  // Method 2: per-stage timing

        // Wrap the member mailbox in a shared_ptr wrapper that doesn't own it
        std::shared_ptr<FrameMailbox> mb_wrapper(
            std::shared_ptr<FrameMailbox>{},
            &mb_face_
        );

        // Use generic inference worker loop (handles frame fetch, inference, visualization, storage)
        inference_worker::run_inference_loop(
            face_runner_.get(),
            mb_wrapper,
            reinterpret_cast<FusionResults*>(&fusion_),
            frame_writer_face_.get(),  // Face model writes output (primary model)
            config,
            stop_face_);

        LG_INFO("face_loop:stop");
    } catch (const std::exception& e) {
        LG_CRIT("face_loop: exception: %s", e.what());
        std::fflush(nullptr);
        std::abort();
    } catch (...) {
        LG_CRIT("face_loop: unknown exception");
        std::fflush(nullptr);
        std::abort();
    }
}

void Orchestrator::yolo_loop_threadfn() noexcept {
    try {
        LG_INFO("yolo_loop:start (YOLOX detection)");

        // Guard: check if yolo model is enabled
        if (!enable_yolo_model_) {
            LG_WARN("yolo_loop: yolo model disabled, exiting thread");
            return;
        }

        if (!yolo_runner_) {
            LG_ERROR("yolo_loop: yolo_runner_ not initialized but enable_yolo_model_=true");
            return;
        }

        // Configure generic worker for object detection
        inference_worker::WorkerConfig config{};
        config.skip_frames = cfg_.yolo_skip_frames;  // 0/1 = every frame (from config)
        config.model_input_width = yolo_runner_->spec().input_size.w;
        config.model_input_height = yolo_runner_->spec().input_size.h;
        config.model_name = "YOLOX";
        config.blur_config = cfg_.blur_config;  // V7.2: Pass blur config for person blur
        config.flip_horizontal = cfg_.flip_horizontal;  // Mirror correction
        config.log_performance = cfg_.log_performance;  // Method 2: per-stage timing

        // Wrap the member mailbox in a shared_ptr wrapper that doesn't own it
        std::shared_ptr<FrameMailbox> mb_wrapper(
            std::shared_ptr<FrameMailbox>{},
            &mb_yolo_
        );

        // V6.2.3.5.7: Pass face_runner as second_runner so YOLOX worker draws both models
        // Use generic inference worker loop (handles frame fetch, inference, visualization, storage)
        inference_worker::run_inference_loop(
            yolo_runner_.get(),
            mb_wrapper,
            reinterpret_cast<FusionResults*>(&fusion_),
            frame_writer_yolo_.get(),  // YOLOX writer draws combined output (face + person)
            config,
            stop_yolo_,
            face_runner_.get());  // Second runner for combined visualization

        LG_INFO("yolo_loop:stop");
    } catch (const std::exception& e) {
        LG_CRIT("yolo_loop: exception: %s", e.what());
        std::fflush(nullptr);
        std::abort();
    } catch (...) {
        LG_CRIT("yolo_loop: unknown exception");
        std::fflush(nullptr);
        std::abort();
    }
}
