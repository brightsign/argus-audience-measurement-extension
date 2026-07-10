#ifndef RUNTIME_SETTINGS_H
#define RUNTIME_SETTINGS_H

#include <cstdint>
#include <cstdlib>

struct RuntimeSettings {
  // Performance vs quality trade-offs
  int   target_fps{30};              // 0 = as fast as possible
  bool  drop_frames_on_lag{true};    // prefer latency over throughput
  bool  use_rga{true};               // hardware preproc
  bool  zero_copy{true};             // try to avoid copies where possible

  // Frame skipping (per model). Process every Nth frame; skipped frames are
  // captured but not run through the model, so they cost almost nothing.
  //   0 or 1 = every frame (max accuracy, highest load)
  //   2      = every 2nd frame (~half the NPU+CPU work for that model)
  //   3      = every 3rd frame, ... N = effective rate is capture_fps / N
  // Trade-off: higher N => lower CPU/NPU/heat but fewer detections/sec and
  // slower reaction to fast movement. On single-NPU-core SoCs (LS5/RK3568)
  // both models share one NPU, so skipping the pricier YOLOX also speeds up
  // RetinaFace by reducing contention.
  int   face_skip_frames{0};         // RetinaFace: 0/1 = all frames
  int   yolo_skip_frames{0};         // YOLOX: 0/1 = all frames

  // Threading/affinity (optional)
  int   worker_affinity{-1};         // -1 = no pinning
  int   preproc_affinity{-1};
  int   inference_affinity{-1};

  // Batching & queues
  int   max_queue_frames{2};         // between stages
  bool  enable_profiling{false};     // lightweight timers

  // Timeouts (ms)
  int   heartbeat_ms{1000};
  int   input_starvation_ms{800};    // no frame arrival => starvation
  int   stage_timeout_ms{1500};      // generic stage timeout

  bool validate(char* err, size_t err_sz) const noexcept;
};

#endif // RUNTIME_SETTINGS_H

