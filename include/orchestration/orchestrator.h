#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include <atomic>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include "input/input_factory.h"
#include "health/health_manager.h"
#include "config/model_spec.h"
#include "models/model_runner.h"
#include "pipeline/shared_frame.h"
#include "pipeline/frame_mailbox.h"
#include "output/frame_writer.h"
#include "config/publisher_config.h"
#include "output/publisher_v2.h"
#include "tracking/tracker.h"

// Optional: describe how to build the pipeline
struct PipelineConfig {
  InputConfig input;           // from your existing config
  // Primary and secondary model specs
  ModelSpec primary_model{};   // Usually RetinaFace (face + gaze)
  ModelSpec secondary_model{}; // Usually YOLOX (objects/people)
  
  int heartbeat_timeout_ms{1500}; // if worker misses heartbeats -> restart
  
  // Test mode: control which models are enabled (default: both enabled)
  bool enable_face_model = true;   // Enable RetinaFace on NPU core 0
  bool enable_yolo_model = true;   // Enable YOLOX on NPU core 1
  
  // Frame output options (optional)
  bool enable_frame_output = false; // Enable decorated frame writing
  std::string output_dir;           // Directory for decorated frames
  int max_frames = 0;               // Max frames to keep (0 = no limit)
  int frame_quality = 85;           // JPEG quality (1-100)
  
  // Publishers configuration
  std::vector<PublisherConfig> publishers; // MQTT, UDP, File publishers
};

enum class OrchestratorState : uint8_t {
  Stopped, Starting, Running, Degraded, Recovering, Error
};

class Orchestrator {
public:
  explicit Orchestrator(PipelineConfig cfg) noexcept;
  ~Orchestrator();

  // not copyable/movable
  Orchestrator(const Orchestrator&) = delete;
  Orchestrator& operator=(const Orchestrator&) = delete;

  bool start() noexcept;                // spawn supervisor+worker
  void request_stop() noexcept;         // ask threads to exit
  void join() noexcept;                 // wait for threads

  OrchestratorState state() const noexcept { return state_.load(std::memory_order_acquire); }

  // Hot swap input (e.g., RTSP URL changed or USB device switched)
  // Will trigger a controlled rebuild of the pipeline.
  bool switch_input(const InputConfig& new_input) noexcept;

  // Read-only snapshots for logging/metrics UIs
  HealthSnapshot source_health() const noexcept { return source_health_.snapshot(); }

  // Helper: safe thread join (only if joinable)
  static void join_if(std::thread& t) noexcept {
    if (t.joinable()) t.join();
  }

private:
  // ---- worker lifecycle ----
  bool build_pipeline() noexcept;       // create InputSource + stage objects
  void destroy_pipeline() noexcept;     // free in correct order
  bool start_threads_after_build() noexcept;  // launch capture + model threads
  void stop_threads() noexcept;               // stop all worker threads cleanly

  // ---- threads ----
  void supervisor_loop() noexcept;      // monitors health + heartbeats, triggers recovery
  
  // New multi-model worker threads
  void capture_loop_threadfn() noexcept;     // Reads camera, fans out to mailboxes
  void face_loop_threadfn() noexcept;        // RetinaFace on NPU core 0
  void yolo_loop_threadfn() noexcept;   // YOLOX detection loop

  // ---- recovery helpers ----
  void mark_broken(FaultCode code, int64_t now_ns) noexcept;
  bool recover_pipeline(int64_t now_ns) noexcept;

private:
  PipelineConfig cfg_;
  std::atomic<OrchestratorState> state_{OrchestratorState::Stopped};

  // ---- Input source ----
  std::shared_ptr<IInputSource> input_;

  // ---- Model enablement flags (set during build_pipeline) ----
  bool enable_face_model_{true};   // RetinaFace enabled?
  bool enable_yolo_model_{true};   // YOLOX enabled?

  // ---- Model runners (multi-model) ----
  // Primary: RetinaFace for face/gaze detection (NPU core 0)
  std::shared_ptr<IModelRunner> face_runner_;
  
  // Secondary: YOLOX for object/person detection (NPU core 0 and 1)
  // NEW: Dual YOLOX contexts for parallel core utilization
  std::shared_ptr<IModelRunner> yolo_runner_;   // YOLOX on NPU

  // ---- Frame fan-out mailboxes (lock-free) ----
  FrameMailbox mb_face_;   // For RetinaFace worker

  // ---- YOLOX shared state (double-buffer pattern) ----
  // Preprocess frame into one of two 640×640 RGB buffers
  FrameMailbox mb_yolo_;           // YOLOX frame queue

  // ---- Worker threads ----
  std::thread supervisor_th_;
  std::thread capture_th_;
  std::thread face_th_;
  std::thread yolo_th_;            // YOLOX thread

  // ---- Stop flags for each thread ----
  std::atomic<bool> orchestrator_stop_{false};  // Supervisor loop
  std::atomic<bool> stop_capture_{false};       // Capture thread
  std::atomic<bool> stop_face_{false};          // RetinaFace thread
  std::atomic<bool> stop_yolo_{false};          // NEW: YOLOX workers

  // ---- Frame sequencing ----
  std::atomic<uint64_t> frame_seq_{0};

  // ---- Fusion state: shared analytics result from both models ----
  struct FusionState {
    std::mutex m;

    // Face/gaze results (from RetinaFace thread)
    std::vector<Detection> face_dets;
    std::vector<Landmarks> face_lms;
    uint64_t face_seq{0};

    // Object/person detections (from YOLOX thread)
    std::vector<Detection> yolo_dets;
    uint64_t yolo_seq{0};

    // Person tracks with stable IDs
    std::vector<TrackedBox> tracks;
    
    // V6.2: Frame dimensions for normalized speed
    int frame_width{640};
    int frame_height{480};
  } fusion_;

  // health/backoff for the source pipeline
  HealthManager source_health_{SourceKind::Unknown};

  // heartbeat from capture thread → supervisor
  std::atomic<int64_t> last_heartbeat_ns_{0};

  // Frame writers for decorated output (optional) - separate per model
  std::unique_ptr<IFrameWriter> frame_writer_face_;   // RetinaFace output
  std::unique_ptr<IFrameWriter> frame_writer_yolo_;   // YOLOX output
  
  // Publishers for analytics (MQTT, UDP, etc.)
  std::vector<PublisherPtr> publishers_;

  // Person tracker with stable GUID assignment
  Tracker person_tracker_{TrackerConfig{
    .iou_match_thresh    = 0.35f,       // IoU threshold for association
    .confirm_hits        = 3,           // Need 3 hits to confirm
    .max_missed          = 12,          // ~0.4s @ 30fps before deletion
    .min_det_score       = 0.50f,       // Raised to 0.50 for quality
    .min_area_px         = 1600,        // Kill tiny boxes (~40x40)
    .motion_eps_px       = 0.8f,        // V6.2.2: Paired with fps_for_motion=30
    .motion_deadband_px  = 2.0f,        // V6.2.2: Relaxed for 1 Hz updates
    .smooth_pos_alpha    = 0.60f,       // Quicker response (reduced from 0.70)
    .smooth_vel_alpha    = 0.55f,       // V6.2.2: Slightly increased for stability
    .dir_hysteresis_deg  = 22.5f,       // Direction change threshold
    .bin_hysteresis_deg  = 8.0f,        // V6.2.2: Snappier L/R switching
    .class_person        = 0,           // YOLOX person class
    .max_center_dist_px  = 80.f,        // Distance fallback for association
    .min_speed_px_s      = 3.0f,        // Motion floor in px/s (reduced from 6.0)
    .publish_grace_missed = 6,          // Publish up to 6 misses (increased from 4)
    .enter_exit_border_frac = 0.10f,    // 10% inset for ROI
    .dir_hold_ms         = 300.0,       // V6.2: Keep direction for 300ms when slow
    .dir_decay_per_s     = 0.5f,        // V6.2: Confidence decay rate
    .low_score_thresh    = 0.80f        // V6.2: Dampen direction when score < 0.80
  }};

  // Emission cache to hold last non-empty tracks (prevents people:0 gaps)
  struct EmitCache {
    std::vector<TrackedBox> last_nonempty;
    double last_nonempty_ts{0.0};
  } emit_cache_;
  
  // Gaze time accumulator per track ID
  std::unordered_map<int, double> gaze_time_map_;

  // Device and stream identifiers for MQTT namespacing
  std::string device_id_;          // e.g., "xt5-01"
  std::string stream_id_;          // e.g., "/dev/video1"

  // cached input kind for HealthManager
  SourceKind detect_source_kind(const InputConfig& ic) const noexcept {
    if (!ic.rtsp_url.empty()) return SourceKind::RTSP;
    if (!ic.usb_device.empty()) return SourceKind::USB;
    if (!ic.file_path.empty()) return SourceKind::File;
    return SourceKind::Unknown;
  }
};

#endif // ORCHESTRATOR_H
