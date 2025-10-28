#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include <atomic>
#include <memory>
#include <thread>
#include <string>
#include "input/input_factory.h"
#include "health/health_manager.h"
#include "config/model_spec.h"
#include "models/model_runner.h"

// Optional: describe how to build the pipeline
struct PipelineConfig {
  InputConfig input;           // from your existing config
  // add preprocess / model configs as needed
  int heartbeat_timeout_ms{1500}; // if worker misses heartbeats -> restart
  ModelSpec model{};
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

private:
  // ---- worker lifecycle ----
  bool build_pipeline() noexcept;       // create InputSource + stage objects
  void destroy_pipeline() noexcept;     // free in correct order
  bool start_worker() noexcept;         // start frame loop thread
  void stop_worker() noexcept;

  // ---- threads ----
  void supervisor_loop() noexcept;      // monitors health + heartbeats, triggers recovery
  void worker_loop() noexcept;          // runs capture->infer loop (wrapper)
  void worker_loop_threadfn(
      std::shared_ptr<IInputSource> in,
      std::shared_ptr<IModelRunner> run) noexcept;  // actual worker logic

  // ---- recovery helpers ----
  void mark_broken(FaultCode code, int64_t now_ns) noexcept;
  bool recover_pipeline(int64_t now_ns) noexcept;

private:
  PipelineConfig cfg_;
  std::atomic<OrchestratorState> state_{OrchestratorState::Stopped};

  // pipeline (now shared_ptr so zombie threads can keep resources alive safely)
  std::shared_ptr<IInputSource> input_;
  // add preprocess/model runners here (shared_ptr<...>)

  // health/backoff for the source pipeline (one per input)
  HealthManager source_health_{SourceKind::Unknown};

  // threads & control flags
  std::thread supervisor_th_;
  std::thread worker_th_;
  std::atomic<bool> orchestrator_stop_{false};  // controls supervisor loop lifetime
  std::atomic<bool> stop_worker_flag_{false};   // controls ONLY current worker thread loop
  std::atomic<bool> worker_exited_{true};  // true when worker thread has exited; allows non-blocking join check

  // heartbeat from worker → supervisor
  std::atomic<int64_t> last_heartbeat_ns_{0};

  // cached input kind for HealthManager
  SourceKind detect_source_kind(const InputConfig& ic) const noexcept {
    if (!ic.rtsp_url.empty()) return SourceKind::RTSP;
    if (!ic.usb_device.empty()) return SourceKind::USB;
    if (!ic.file_path.empty()) return SourceKind::File;
    return SourceKind::Unknown;
  }
  std::shared_ptr<IModelRunner> runner_;
};

#endif // ORCHESTRATOR_H
