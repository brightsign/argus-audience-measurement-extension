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

