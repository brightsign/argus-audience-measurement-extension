# C++ Software Architecture Design Document

**Project:** Argus Audience Measurement Extension
**Purpose:** Real-time face detection, gaze tracking, and person tracking on BrightSign players with Rockchip NPU
**Target Audience:** Human developers and AI agents modifying the codebase
**Last Updated:** 2026-01-06

---

## Table of Contents

1. [System Overview](#system-overview)
2. [High-Level Architecture](#high-level-architecture)
3. [Threading Model](#threading-model)
4. [Synchronization Primitives](#synchronization-primitives)
5. [Data Flow Pipeline](#data-flow-pipeline)
6. [Core Components](#core-components)
7. [Memory Management](#memory-management)
8. [Error Handling & Recovery](#error-handling--recovery)
9. [Performance Characteristics](#performance-characteristics)
10. [Code Modification Guide](#code-modification-guide)

---

## System Overview

### What This System Does

This is a **production-grade edge AI application** that performs real-time computer vision on BrightSign media players equipped with Rockchip NPU (Neural Processing Unit) hardware acceleration. The system:

1. **Captures video frames** from USB cameras, RTSP streams, or video files
2. **Detects faces** using RetinaFace neural network (with 5-point facial landmarks)
3. **Detects people/objects** using YOLOX neural network
4. **Tracks people** across frames with stable IDs using ByteTrack or legacy EMA-based tracker
5. **Analyzes gaze direction** to determine if people are looking at the camera
6. **Publishes analytics** via MQTT, UDP, or file sinks (JSON format)
7. **Writes annotated frames** for debugging and verification (optional)
8. **Recovers automatically** from camera disconnects, network failures, and other transient errors

### Key Technologies

- **C++20** (primary language)
- **OpenCV 4.x** (image processing, color conversion, drawing)
- **GStreamer 1.0** (RTSP stream capture)
- **RKNN SDK** (Rockchip NPU inference)
- **Mosquitto MQTT** (analytics publishing)
- **TurboJPEG** (fast JPEG encoding for frame output)

### Build System

- **CMake** 3.x for cross-compilation (x86_64 → aarch64)
- **Yocto SDK** for embedded Linux toolchain
- **Makefile** wrapper for simplified build orchestration

---

## High-Level Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                      Main Thread                            │
│  ┌──────────────┐                                           │
│  │   main()     │  - Parse config                           │
│  │              │  - Build pipeline                         │
│  │              │  - Signal handling (SIGINT/SIGTERM)       │
│  └──────┬───────┘                                           │
│         │                                                    │
│         ▼                                                    │
│  ┌──────────────────────────────────────────────────────┐  │
│  │           Orchestrator                                │  │
│  │  - Lifecycle management                               │  │
│  │  - Thread spawning/joining                            │  │
│  │  - Pipeline construction/destruction                  │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                          │
        ┌─────────────────┼─────────────────┐
        │                 │                 │
        ▼                 ▼                 ▼
┌────────────┐    ┌────────────┐    ┌────────────┐
│ Supervisor │    │  Capture   │    │  Model     │
│   Thread   │    │  Thread    │    │ Threads    │
│            │    │            │    │ (2-3x)     │
└────────────┘    └────────────┘    └────────────┘
     │                 │                  │
     │                 │                  ├─ Face Thread (RetinaFace)
     │                 │                  └─ YOLO Thread (YOLOX)
     │                 │
     ▼                 ▼
┌────────────┐    ┌──────────────────────┐
│  Health    │    │  Input Sources       │
│  Monitor   │◄───┤  - USB Camera (V4L2) │
│            │    │  - RTSP (GStreamer)  │
└────────────┘    │  - File Playback     │
     │            └──────────────────────┘
     ▼
┌────────────┐
│  Recovery  │
│  Logic     │
└────────────┘
```

### Orchestrator State Machine

```
Stopped ──start()──► Starting ──build_pipeline()──► Running
                         │                            │
                         │ (failure)                  │
                         ▼                            ▼
                      Error                      Degraded
                                                     │
                                                     ▼
                                                Recovering ◄─┐
                                                     │       │
                                      (success)─────┘       │
                                                             │
                                      (failure)──────────────┘
```

---

## Threading Model

### Thread Architecture Overview

The system uses **4+ concurrent threads** for parallel processing:

1. **Main Thread** - Configuration, initialization, signal handling
2. **Supervisor Thread** - Health monitoring, recovery, analytics publishing
3. **Capture Thread** - Frame acquisition from camera/stream
4. **Face Thread** - RetinaFace inference on NPU core 0 (optional)
5. **YOLO Thread** - YOLOX inference on NPU core 1 (optional)

### Thread 1: Main Thread

**Location:** `src/main.cpp:main()`
**Lifespan:** Application start → exit
**Responsibilities:**
- Install crash handlers (SIGSEGV, SIGABRT, etc.)
- Parse CLI arguments and load JSON configuration
- Initialize file logger (`/storage/sd/logs/gaze.log`)
- Create Orchestrator instance
- Install signal handlers (SIGINT/SIGTERM)
- Sleep loop waiting for stop signal (`std::atomic<bool> g_stop`)
- Request orchestrator shutdown and join all threads

**Key Code:**
```cpp
// main.cpp:380-387
std::signal(SIGINT, on_sig);
std::signal(SIGTERM, on_sig);
while (!g_stop.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

orch.request_stop();
orch.join();
```

### Thread 2: Supervisor Thread

**Location:** `src/orchestration/orchestrator.cpp:supervisor_loop()`
**Spawned:** `Orchestrator::start()` line 178
**Lifespan:** Orchestrator start → stop
**Responsibilities:**

1. **Health Monitoring** (every 100ms):
   - Check heartbeat from capture thread (`last_heartbeat_ns_`)
   - Timeout threshold: 3000ms (USB) or configurable
   - Mark source as broken if heartbeat stale

2. **Recovery Orchestration**:
   - Detect broken state from HealthManager
   - Implement exponential backoff (250ms → 8000ms)
   - Call `recover_pipeline()` to rebuild input + models
   - Reset tracker state after successful recovery

3. **Analytics Publishing** (every 1 second):
   - Fuse face and YOLOX detections from `FusionState`
   - Update person tracker with filtered detections
   - Match faces to person tracks via IoU + head expansion
   - Calculate gaze direction using facial landmarks
   - Publish results to all configured publishers (MQTT/UDP/File)
   - Emit track cache to prevent empty gaps (500ms hold)

4. **Logging & Diagnostics**:
   - Log health status every 5 seconds
   - Track frame processing rate (FPS calculation)
   - Count people and gaze events

**Synchronization:**
- **Reads:** `last_heartbeat_ns_` (atomic, relaxed ordering)
- **Reads:** `source_health_` (HealthManager with internal atomics)
- **Writes:** `state_` (atomic, release ordering)
- **Locks:** `fusion_.m` (mutex) to read face/YOLO detections

**Stop Condition:** `orchestrator_stop_.load(std::memory_order_acquire)`

### Thread 3: Capture Thread

**Location:** `src/orchestration/orchestrator.cpp:capture_loop_threadfn()`
**Spawned:** `start_threads_after_build()` line 407
**Lifespan:** After pipeline build → stop signal
**Responsibilities:**

1. **Frame Acquisition**:
   - Call `input_->tryFetch(camView)` to get next frame
   - Handle `FetchStatus::Ok` / `FetchStatus::Timeout` / `FetchStatus::Error`
   - Report health events: `onFrameOk()` or `onBusError()`

2. **Frame Wrapping**:
   - Wrap FrameView into SharedFrame (zero-copy buffer wrap)
   - Assign sequence number: `frame_seq_.fetch_add(1)`
   - Copy BGR pixel data (320×320 or 640×640 preprocessed)
   - Preserve original dimensions (`orig_width`, `orig_height`)

3. **Frame Fan-Out** (lock-free):
   - Post to face mailbox: `mb_face_.postFrame(sf)` if face model enabled
   - Post to YOLO mailbox: `mb_yolo_.postFrame(sf)` if YOLO model enabled
   - Uses atomic operations (no mutex)

4. **Heartbeat Update**:
   - Write `last_heartbeat_ns_.store(now_ns())` every frame
   - Supervisor monitors this to detect starvation

**Synchronization:**
- **Writes:** `frame_seq_` (atomic, relaxed)
- **Writes:** `last_heartbeat_ns_` (atomic, release)
- **Writes:** `mb_face_`, `mb_yolo_` (lock-free mailboxes)

**Stop Condition:** `stop_capture_.load(std::memory_order_relaxed)`

**Sleep on Error:** 5ms sleep on fetch failure to avoid busy-wait

### Thread 4: Face Thread (RetinaFace)

**Location:** `src/orchestration/orchestrator.cpp:face_loop_threadfn()`
**Spawned:** `start_threads_after_build()` line 422
**Lifespan:** After pipeline build → stop signal (only if `enable_face_model_` is true)
**Responsibilities:**

1. **Frame Consumption**:
   - Poll `mb_face_.takeFrame()` for new frames
   - Sleep 5ms if nullptr (no new frame)

2. **Inference Execution**:
   - Run RetinaFace model on NPU core 0
   - Extract face bounding boxes and 5-point landmarks
   - Apply confidence threshold (typically 0.5)

3. **Result Storage**:
   - Lock `fusion_.m` mutex
   - Update `fusion_.face_dets`, `fusion_.face_lms`, `fusion_.face_seq`
   - Unlock immediately (minimize critical section)

4. **Optional Visualization**:
   - Draw face boxes and landmarks on frame
   - Write to `frame_writer_face_` (disabled in production, YOLO draws combined output)

**Synchronization:**
- **Reads:** `mb_face_` (lock-free mailbox)
- **Writes:** `fusion_` (mutex-protected shared state)
- **Writes:** `frame_writer_face_` (optional, thread-local)

**Stop Condition:** `stop_face_.load(std::memory_order_relaxed)`

**NPU Affinity:** Core 0 (configured via `ModelSpec::npu_core`)

### Thread 5: YOLO Thread (YOLOX)

**Location:** `src/orchestration/orchestrator.cpp:yolo_loop_threadfn()`
**Spawned:** `start_threads_after_build()` line 438
**Lifespan:** After pipeline build → stop signal (only if `enable_yolo_model_` is true)
**Responsibilities:**

1. **Frame Consumption**:
   - Poll `mb_yolo_.takeFrame()` for new frames
   - Sleep 5ms if nullptr

2. **Inference Execution**:
   - Run YOLOX model on NPU core 1
   - Detect objects (80 COCO classes)
   - Filter for person class (class_id == 0)
   - Apply confidence threshold (typically 0.5)

3. **Result Storage**:
   - Lock `fusion_.m` mutex
   - Update `fusion_.yolo_dets`, `fusion_.yolo_seq`
   - Unlock immediately

4. **Combined Visualization**:
   - Draw BOTH face and YOLOX detections on frame
   - Read face data from `fusion_` (with lock)
   - Write annotated frames to `frame_writer_yolo_`

**Synchronization:**
- **Reads:** `mb_yolo_` (lock-free mailbox)
- **Reads/Writes:** `fusion_` (mutex-protected shared state)
- **Writes:** `frame_writer_yolo_` (thread-local)

**Stop Condition:** `stop_yolo_.load(std::memory_order_relaxed)`

**NPU Affinity:** Core 1 (configured via `ModelSpec::npu_core`)

### Thread Lifecycle Management

**Start Sequence:**
```cpp
// orchestrator.cpp:168-180
bool Orchestrator::start() {
  state_.store(OrchestratorState::Starting);
  orchestrator_stop_.store(false);

  if (!build_pipeline()) return false;           // Create input + models
  if (!start_threads_after_build()) return false; // Spawn capture + model threads

  supervisor_th_ = std::thread(&Orchestrator::supervisor_loop, this);
  state_.store(OrchestratorState::Running);
  return true;
}
```

**Stop Sequence:**
```cpp
// orchestrator.cpp:459-526
void Orchestrator::stop_threads() {
  // 1. Set all stop flags (atomic release)
  stop_capture_.store(true);
  stop_face_.store(true);
  stop_yolo_.store(true);

  // 2. Unblock capture thread (USB read() may be blocking)
  if (auto* usb = dynamic_cast<UsbInputSource*>(input_.get())) {
    usb->request_stop();
  }

  // 3. Wait up to 200ms for clean exit
  auto deadline = now() + 200ms;
  while (now() < deadline && any_thread_joinable()) {
    sleep(10ms);
  }

  // 4. Join threads (or detach if stuck)
  join_if(capture_th_);
  join_if(face_th_);
  join_if(yolo_th_);
}
```

---

## Synchronization Primitives

### Lock-Free Data Structures

#### FrameMailbox (1-deep latest-wins)

**Location:** `include/pipeline/frame_mailbox.h`
**Pattern:** Single-producer (capture), single-consumer (model thread)
**Capacity:** 1 frame (overwrite-old policy)

**Implementation:**
```cpp
class FrameMailbox {
  std::shared_ptr<SharedFrame> buffer_{nullptr};  // NOT atomic<shared_ptr>
  std::atomic<bool> has_frame_;

  void postFrame(const std::shared_ptr<SharedFrame>& f) noexcept {
    std::atomic_store_explicit(&buffer_, f, std::memory_order_release);
    has_frame_.store(true, std::memory_order_release);
  }

  std::shared_ptr<SharedFrame> takeFrame() noexcept {
    if (!has_frame_.load(std::memory_order_acquire)) return nullptr;

    auto f = std::atomic_exchange_explicit(
        &buffer_, std::shared_ptr<SharedFrame>{}, std::memory_order_acq_rel);
    has_frame_.store(false, std::memory_order_release);
    return f;
  }
};
```

**Why lock-free?**
- Capture thread must NOT block on slow inference
- Model threads can skip frames if behind (latest-wins)
- Uses atomic `shared_ptr` operations (C++20 or `std::atomic_*` free functions)

**Memory Ordering:**
- `postFrame()`: **release** - ensures frame data is visible before `has_frame_` is set
- `takeFrame()`: **acquire** - ensures `has_frame_` is checked before accessing frame
- Exchange: **acq_rel** - combines both for atomic swap

#### SpscDropOld (Ring Buffer)

**Location:** `include/pipeline/frame_queue.h`
**Pattern:** Single-producer, single-consumer
**Capacity:** Configurable (typically 4-8 frames)
**Policy:** Drop oldest when full

**Implementation:**
```cpp
template <typename T>
class SpscDropOld {
  const size_t cap_;
  std::vector<T> buf_;
  std::atomic<size_t> read_{0}, write_{0};

  void push(const T& v) noexcept {
    size_t w = write_.load(std::memory_order_relaxed);
    size_t r = read_.load(std::memory_order_acquire);
    size_t next = (w + 1) % cap_;

    if (next == r) { // Full, drop oldest
      read_.store((r + 1) % cap_, std::memory_order_release);
    }

    buf_[w] = v;
    write_.store(next, std::memory_order_release);
  }

  bool pop(T& out) noexcept {
    size_t r = read_.load(std::memory_order_relaxed);
    size_t w = write_.load(std::memory_order_acquire);
    if (r == w) return false; // Empty

    out = buf_[r];
    read_.store((r + 1) % cap_, std::memory_order_release);
    return true;
  }
};
```

**Memory Ordering:**
- Producer: **release** on `write_` (data visible before index update)
- Consumer: **acquire** on `write_` (see latest data)
- Indices use **relaxed** for local reads (same-thread optimization)

### Mutex-Protected Shared State

#### FusionState (Multi-Model Results)

**Location:** `orchestrator.cpp:142-160`
**Purpose:** Aggregate face and YOLOX detections for analytics

```cpp
struct FusionState {
  std::mutex m;

  // Face model results (written by face thread)
  std::vector<Detection> face_dets;
  std::vector<Landmarks> face_lms;
  uint64_t face_seq{0};

  // YOLOX results (written by YOLO thread)
  std::vector<Detection> yolo_dets;
  uint64_t yolo_seq{0};

  // Tracker results (written by supervisor)
  std::vector<TrackedBox> tracks;
  int frame_width{640}, frame_height{480};
} fusion_;
```

**Access Pattern:**
- **Model threads:** Short writes (lock, update vector, unlock)
- **Supervisor thread:** Longer read+write (copy data, update tracker, store tracks)

**Critical Section Duration:**
- Face/YOLO threads: ~100 microseconds (vector assignment)
- Supervisor: ~5 milliseconds (includes tracking + IoU matching)

**Why mutex here?**
- Multiple writers (face + YOLO threads)
- Complex data structures (vectors, not trivially copyable)
- Acceptable latency (supervisor runs at 1 Hz)

### Atomic Variables

#### Orchestrator Control Flags

```cpp
std::atomic<OrchestratorState> state_;           // State machine
std::atomic<bool> orchestrator_stop_;            // Supervisor exit
std::atomic<bool> stop_capture_;                 // Capture exit
std::atomic<bool> stop_face_;                    // Face thread exit
std::atomic<bool> stop_yolo_;                    // YOLO thread exit
```

**Memory Ordering:**
- **Write:** `std::memory_order_release` (signal stop to other threads)
- **Read:** `std::memory_order_acquire` (see stop request)

#### Frame Sequencing

```cpp
std::atomic<uint64_t> frame_seq_{0};
```

**Usage:**
```cpp
sf->seq = frame_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
```

**Memory Ordering:** Relaxed (just need unique IDs, no synchronization needed)

#### Heartbeat Monitoring

```cpp
std::atomic<int64_t> last_heartbeat_ns_{0};
```

**Write (Capture):**
```cpp
last_heartbeat_ns_.store(now_ns(), std::memory_order_release);
```

**Read (Supervisor):**
```cpp
int64_t last = last_heartbeat_ns_.load(std::memory_order_relaxed);
```

**Why relaxed read?** Supervisor only cares about approximate recency (3-second timeout)

### ThreadPool (Dynamic Worker Pool)

**Location:** `include/ThreadPool.hpp`
**Pattern:** Producer-consumer with condition variable
**Max Threads:** `std::thread::hardware_concurrency()`

**Synchronization:**
```cpp
std::mutex mutex_;
std::condition_variable cv_;
std::queue<Task> tasks_;
```

**Worker Loop:**
```cpp
void worker() {
  while (true) {
    Task task;
    {
      UniqueLock lock(mutex_);
      ++idleThreads_;

      // Wait for task or timeout (2 seconds)
      bool timedout = !cv_.wait_for(lock, 2s, [this] {
        return quit_ || !tasks_.empty();
      });

      --idleThreads_;

      if (tasks_.empty()) {
        if (quit_) return;              // Shutdown
        if (timedout) {                 // Worker retirement
          --currentThreads_;
          joinFinishedThreads();
          finishedThreadIDs_.push(this_thread::get_id());
          return;
        }
      }

      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();  // Execute outside lock
  }
}
```

**Usage in System:** Not currently used in main pipeline (dedicated threads instead), but available for future async tasks.

---

## Data Flow Pipeline

### Frame Capture to Inference

```
┌──────────────────────────────────────────────────────────────┐
│  Input Source (USB / RTSP / File)                            │
│  - V4L2 read() / GStreamer appsink / fread()                 │
│  - Returns FrameView (NV12 format, 640×480 or 1920×1080)     │
└─────────────────┬────────────────────────────────────────────┘
                  │
                  ▼
        ┌─────────────────────┐
        │  Capture Thread     │
        │  - NV12 → BGR       │
        │  - Letterbox resize │
        │  - Wrap in SharedFrame
        └──────────┬──────────┘
                   │
         ┌─────────┴─────────┐
         │                   │
         ▼                   ▼
   ┌──────────┐        ┌──────────┐
   │ mb_face_ │        │ mb_yolo_ │
   │ (mailbox)│        │ (mailbox)│
   └─────┬────┘        └────┬─────┘
         │                  │
         ▼                  ▼
   ┌───────────┐      ┌───────────┐
   │Face Thread│      │YOLO Thread│
   │ NPU Core0 │      │ NPU Core1 │
   └─────┬─────┘      └─────┬─────┘
         │                  │
         └──────────┬───────┘
                    ▼
           ┌────────────────┐
           │  FusionState   │
           │  (mutex-locked)│
           └────────┬───────┘
                    │
                    ▼
         ┌──────────────────────┐
         │  Supervisor Thread   │
         │  - Tracker update    │
         │  - Gaze matching     │
         │  - MQTT publish      │
         └──────────────────────┘
```

### Frame Structure Lifecycle

#### 1. FrameView (Non-Owning)

**Location:** `include/input/input_source.h`
**Purpose:** Zero-copy view into input buffer

```cpp
struct FrameView {
  const uint8_t* plane0;  // Y plane (NV12) or BGR data
  const uint8_t* plane1;  // UV plane (NV12)
  int width, height;      // Preprocessed dimensions (e.g., 320×320)
  int orig_width, orig_height; // Original source dimensions
  int stride0, stride1;   // Row strides
  int64_t pts_ns;         // Presentation timestamp
};
```

**Lifetime:** Scope of `tryFetch()` call (stack or input buffer pool)

#### 2. SharedFrame (Owning, Shared)

**Location:** `include/pipeline/shared_frame.h`
**Purpose:** Reference-counted frame for fan-out

```cpp
struct SharedFrame {
  std::vector<uint8_t> bgr;  // Owned BGR pixel data
  int width, height;         // 320×320 or 640×640
  int orig_width, orig_height; // Source dimensions
  int64_t pts_ns;
  uint64_t seq;              // Monotonic sequence number
};
```

**Sharing:** `std::shared_ptr<SharedFrame>` used in mailboxes
**Copy Semantics:** Only one pixel copy (NV12 → BGR in capture thread)
**Zero-Copy Ideal:** Future optimization could use DMA buffers with hardware colorspace conversion

### Coordinate Space Transforms

**Challenge:** Detections are in model input space (320×320), but tracking and MQTT need source space (1920×1080)

**Transform Types:**

1. **Letterbox (Source → Model)**
   ```cpp
   float scale = min(model_size / src_w, model_size / src_h);
   int fit_w = src_w * scale;
   int fit_h = src_h * scale;
   int pad_x = (model_size - fit_w) / 2;
   int pad_y = (model_size - fit_h) / 2;
   ```

2. **De-Letterbox (Model → Source)**
   ```cpp
   // orchestrator.cpp:686-691
   float cam_x = (model_x - pad_x) / scale;
   float cam_y = (model_y - pad_y) / scale;
   ```

**Where Applied:**
- Face detections: Supervisor de-letterboxes before IoU matching
- YOLOX detections: Already de-letterboxed by postprocessor
- Tracker: Works in source coordinate space
- Visualization: Draws in model space, published in source space

---

## Core Components

### Orchestrator

**Location:** `src/orchestration/orchestrator.cpp` (1415 lines)
**Purpose:** Central coordinator for entire system

**Responsibilities:**
1. **Lifecycle Management:** Start, stop, join all threads
2. **Pipeline Construction:** Create input sources, load models, configure publishers
3. **Health Monitoring:** Track capture heartbeat, detect broken states
4. **Recovery:** Automatic rebuild on camera disconnect or stream failure
5. **Analytics Fusion:** Merge face + YOLOX results, update tracker, publish

**Key State:**
```cpp
OrchestratorState state_;       // Stopped/Starting/Running/Degraded/Recovering/Error
PipelineConfig cfg_;            // Configuration snapshot
std::shared_ptr<IInputSource> input_;
std::shared_ptr<IModelRunner> face_runner_;
std::shared_ptr<IModelRunner> yolo_runner_;
HealthManager source_health_;
Tracker person_tracker_;
std::vector<PublisherPtr> publishers_;
```

**State Transitions:**
- `start()` → Starting → Running
- Heartbeat timeout → Degraded → Recovering
- Recovery success → Running
- Recovery failure → Degraded (retry with backoff)

### Input Sources

**Interface:** `include/input/input_source.h`

```cpp
class IInputSource {
  virtual bool open() noexcept = 0;
  virtual bool start() noexcept = 0;
  virtual FetchStatus tryFetch(FrameView& out) noexcept = 0;
  virtual void stop() noexcept = 0;
  virtual void close() noexcept = 0;
};
```

#### USB Camera (V4L2)

**Location:** `src/input/input_usb.cpp`
**Backend:** Linux V4L2 API (`/dev/videoN`)
**Format:** NV12 (hardware colorspace)
**Challenges:**
- Slow capture rate (~5 fps on some hardware)
- Blocking `read()` requires `request_stop()` to unblock
- Device enumeration via `stat()` + trial open

**Frame Preprocessing:**
1. V4L2 `read()` → NV12 buffer
2. Software NV12 → BGR conversion
3. Letterbox resize to 320×320

#### RTSP Stream (GStreamer)

**Location:** `src/input/input_rtsp.cpp`
**Backend:** GStreamer 1.0 pipeline
**Format:** H.264 decode to NV12 via hardware decoder

**Pipeline:**
```
rtspsrc → rtph264depay → h264parse → mppvideodec → videoconvert → appsink
```

**Challenges:**
- Network latency (60+ seconds for DHCP + stream open)
- Bus errors (EOS, state changes) require recovery
- Appsink starvation detection (3-second timeout)

**GStreamer Message Handling:**
```cpp
// Bus callback on separate thread
void on_bus_message(GstMessage* msg) {
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR:
      health_manager_.onBusError(now_ns(), error_string);
      break;
    case GST_MESSAGE_EOS:
      health_manager_.onBusEos(now_ns());
      break;
  }
}
```

#### File Input

**Location:** `src/input/input_file.cpp`
**Backend:** OpenCV VideoCapture
**Modes:** One-shot or looping playback
**Use Case:** Offline testing, reproducible benchmarks

### Model Runners

**Interface:** `include/models/model_runner.h`

```cpp
class IModelRunner {
  virtual bool load(const ModelSpec& spec) noexcept = 0;
  virtual bool infer(const FrameView& in, InferenceOutputs& out) noexcept = 0;
  virtual void unload() noexcept = 0;
  virtual const ModelSpec& spec() const noexcept = 0;
  virtual int64_t last_infer_ns() const noexcept = 0; // Timing
};

struct InferenceOutputs {
  const Detection* dets;    // Non-owning pointer
  int num_dets;
  const Landmarks* lms;     // For faces
  int num_lms;
};
```

#### RetinaFace Runner

**Location:** `src/models/model_runner_retinaface.cpp`
**Model:** RetinaFace (face detection + 5-point landmarks)
**Input:** 320×320 RGB (normalized)
**Output:**
- Face bounding boxes (x0, y0, x1, y1)
- 5-point landmarks (left_eye, right_eye, nose, left_mouth, right_mouth)
- Confidence scores

**NPU Execution:**
```cpp
rknn_inputs_set(ctx_, 0, &inputs[0]); // Upload preprocessed RGB
rknn_run(ctx_, nullptr);              // Execute on NPU
rknn_outputs_get(ctx_, 3, outputs);   // Get 3 output tensors
```

**Postprocessing:**
1. Decode anchors (Faster R-CNN style)
2. Apply confidence threshold (0.5)
3. NMS (IoU threshold 0.4)
4. De-quantize landmark coordinates

#### YOLOX Runner

**Location:** `src/models/model_runner_yolox.cpp`
**Model:** YOLOX-S (COCO 80 classes)
**Input:** 640×640 BGR (normalized)
**Output:**
- Object bounding boxes (80 classes)
- Class 0 = person (COCO dataset)
- Confidence scores

**Postprocessing:**
1. Decode grid predictions
2. Filter by score (0.5 threshold)
3. NMS per class (IoU 0.45)
4. De-letterbox to source coordinates

### Person Tracker

**Location:** `src/tracking/tracker.cpp`
**Purpose:** Assign stable IDs across frames

**Algorithms:**
1. **ByteTrack** (default) - Kalman filter + two-stage matching
2. **Legacy EMA** - Exponential moving average + IoU matching

**Tracking Features:**
- Stable GUID assignment (1, 2, 3, ...)
- Motion estimation (velocity, direction)
- Dwell time accumulation
- Enter/exit event detection
- Direction quantization (8-way: R, UR, U, UL, L, DL, D, DR)
- Gaze time accumulation (when face matched)

**Association Algorithm (Legacy):**
```cpp
// tracker.cpp:assign_tracks()
1. Compute IoU matrix (N_detections × M_tracks)
2. Greedy matching: pick best IoU > threshold (0.45)
3. Unmatched detections → new tentative tracks
4. Unmatched tracks → increment missed counter
5. Tracks with missed > max_missed (12) → delete
```

**Motion Estimation:**
```cpp
// Smooth position with EMA
cx_smooth = alpha * cx_new + (1-alpha) * cx_prev;
cy_smooth = alpha * cy_new + (1-alpha) * cy_prev;

// Calculate velocity from position history
float dt = ts_hist[now] - ts_hist[prev];  // Real time delta
vx = (cx_smooth - cx_prev) / dt;
vy = (cy_smooth - cy_prev) / dt;
speed = sqrt(vx*vx + vy*vy);
dir_deg = atan2(-vy, vx) * 180/PI;  // -vy because screen Y is down
```

**Direction Quantization:**
```
   270 (U)
     |
225--+--315
(UL) | (UR)
     |
180--+--0 (R)
(L)  |
     |
135--+--45
(DL) | (DR)
     |
   90 (D)
```

### Health Manager

**Location:** `include/health/health_manager.h`
**Purpose:** Track source health and compute recovery backoff

**Event Types:**
- `onFrameOk(ts)` - Successful frame received
- `onBusError(ts, msg)` - GStreamer error
- `onBusEos(ts)` - GStreamer end-of-stream
- `onAppsinkStarvation(ts)` - Heartbeat timeout
- `onUsbDeviceGone(ts)` - USB device disappeared
- `onUsbReadFailure(ts)` - V4L2 read() failed

**Backoff Strategy:**
```cpp
struct BackoffPolicy {
  int base_ms{250};      // Initial retry delay
  int max_ms{8000};      // Maximum retry delay
  float factor{2.0f};    // Exponential multiplier
  int jitter_ms{100};    // Random jitter
};
```

**Backoff Calculation:**
```cpp
int nextBackoffMs(int64_t now_ns) {
  if (consecutive_failures == 0) return base_ms;

  int ms = base_ms * pow(factor, consecutive_failures - 1);
  ms = min(ms, max_ms);
  ms += random(0, jitter_ms);
  return ms;
}
```

**Usage in Recovery:**
```cpp
// supervisor_loop()
if (is_broken) {
  int64_t elapsed_ms = (now - last_recovery_attempt) / 1'000'000;
  if (elapsed_ms >= recovery_backoff_ms) {
    if (!recover_pipeline(now)) {
      recovery_backoff_ms = min(8000, recovery_backoff_ms * 2);
    } else {
      recovery_backoff_ms = 250; // Reset on success
    }
  }
}
```

### Publishers

**Interface:** `include/output/publisher_v2.h`

```cpp
class IPublisher {
  virtual bool start() noexcept = 0;
  virtual void publish_result(const PipelineResult& result) noexcept = 0;
  virtual void stop() noexcept = 0;
};

struct PipelineResult {
  int people_count;
  int gaze_count;
  std::vector<TrackedBox> person_tracks;
  uint64_t seq;
  int fps;
  uint64_t ts_ns;
  int frame_width, frame_height;
};
```

#### MQTT Publisher (BrightSign V3 Protocol)

**Location:** `src/output/brightsign_v3_publisher.cpp`
**Protocol:** JSON over MQTT (Mosquitto)
**Topic Structure:**
```
brightsign/v3/{device_id}/{stream_id}/analytics
```

**Message Format:**
```json
{
  "people": 2,
  "gaze": 1,
  "fps": 8,
  "seq": 1234,
  "ts": 1704567890123,
  "tracks": [
    {
      "id": 1,
      "bbox": [100, 200, 300, 400],
      "score": 0.95,
      "velocity": [12.5, -3.2],
      "speed": 12.9,
      "direction": "R",
      "direction_confidence": 0.88,
      "dwell_s": 4.5,
      "has_gaze": true,
      "is_gazing": true,
      "gaze_time": 2.3,
      "face_bbox": [150, 210, 250, 310],
      "state": "confirmed"
    }
  ]
}
```

**Async Wrapper:**
```cpp
// output/async_publisher.cpp
class AsyncPublisher {
  std::thread worker_;
  SpscDropOld<PipelineResult> queue_{64};

  void worker_loop() {
    while (!stop_) {
      PipelineResult result;
      if (queue_.pop(result)) {
        delegate_->publish_result(result);
      } else {
        sleep(5ms);
      }
    }
  }
};
```

#### UDP JSON Publisher

**Location:** `src/output/udp_json_publisher.cpp`
**Protocol:** JSON over UDP broadcast
**Port:** Configurable (default 9999)
**Use Case:** Local network monitoring

#### File Sink Publisher

**Location:** `src/output/file_sinks.cpp`
**Format:** JSON lines (one result per line)
**Use Case:** Offline analysis, debugging

---

## Memory Management

### Zero-Copy Design

**Goal:** Minimize pixel data copies (expensive on embedded)

**Copy Points:**
1. **Capture Thread:** NV12 → BGR (unavoidable, hardware format conversion)
2. **Shared Frame:** BGR data copied into `std::vector<uint8_t>` (owned buffer)
3. **Mailbox Fan-Out:** `std::shared_ptr` reference counting (NO pixel copy)
4. **Inference Input:** BGR → RKNN input tensor (DMA if supported, otherwise memcpy)

**Future Optimization:** Use RKNN zero-copy API with RGA (Rockchip Graphics Accelerator) for hardware colorspace conversion and scaling.

### Shared Pointer Lifecycle

```
Capture Thread                Model Thread 1          Model Thread 2
     │                              │                       │
     ├─ new SharedFrame            │                       │
     ├─ shared_ptr<SF> sf          │                       │
     ├─ mb_face_.post(sf)──────────►                       │
     ├─ mb_yolo_.post(sf)──────────┼───────────────────────►
     │                              │                       │
     ▼ (sf goes out of scope)       │                       │
   ref_count = 2                    │                       │
                                    ▼                       ▼
                            auto f = takeFrame()    auto f = takeFrame()
                            infer(f)                infer(f)
                            (f out of scope)        (f out of scope)
                            ref_count = 1           ref_count = 0
                                                    → delete SharedFrame
```

**Key Insight:** Last model thread to finish inference deallocates the frame (automatic via `shared_ptr`).

### Memory Pools (Unused in Current Design)

**Location:** `src/resources/mem_pool.cpp`
**Status:** Available but not currently used
**Future Use:** Pre-allocate frame buffers to avoid dynamic allocation in capture loop

---

## Error Handling & Recovery

### Failure Modes

1. **Camera Disconnect (USB)**
   - Detection: Heartbeat timeout (3 seconds)
   - V4L2 `read()` returns error
   - Health: `onUsbDeviceGone()`
   - Recovery: Rescan `/dev/video*` devices, try next available

2. **Network Failure (RTSP)**
   - Detection: GStreamer bus error or EOS
   - Appsink starvation (no frames for 3+ seconds)
   - Health: `onBusError()`, `onAppsinkStarvation()`
   - Recovery: Rebuild GStreamer pipeline, reconnect stream

3. **NPU Inference Failure**
   - Detection: `rknn_run()` returns error
   - Model thread catches exception, logs critical error
   - Recovery: **ABORT** (fatal, requires restart)

4. **Memory Exhaustion**
   - Detection: `std::bad_alloc` exception
   - Recovery: **ABORT** (cannot proceed safely)

### Recovery Flow

```
┌────────────────────────────────────────┐
│  Supervisor Loop (every 100ms)         │
│  1. Check last_heartbeat_ns_           │
│  2. If stale (>3s), mark broken        │
└───────────────┬────────────────────────┘
                │
                ▼
        ┌───────────────┐
        │  is_broken?   │───No──► Continue
        └───────┬───────┘
                │Yes
                ▼
    ┌────────────────────────┐
    │ Backoff elapsed?       │───No──► Wait
    └───────┬────────────────┘
            │Yes
            ▼
  ┌─────────────────────────┐
  │ recover_pipeline()      │
  │ 1. Stop threads         │
  │ 2. Close input          │
  │ 3. Unload models        │
  │ 4. Rescan devices       │
  │ 5. Rebuild input        │
  │ 6. Reload models        │
  │ 7. Start threads        │
  │ 8. Reset tracker        │
  └────────┬────────────────┘
           │
     ┌─────┴─────┐
     │  Success? │
     └─────┬─────┘
           │
      ┌────┴────┐
      │Yes      │No
      ▼         ▼
 Clear Broken  Increase Backoff
 State=Running  (250ms→500ms→1s→2s→4s→8s)
```

### Crash Handling

**Signal Handlers:**
```cpp
// main.cpp:22-41
std::signal(SIGSEGV, sig_handler);  // Segmentation fault
std::signal(SIGABRT, sig_handler);  // Abort
std::signal(SIGFPE,  sig_handler);  // Floating point exception
std::signal(SIGILL,  sig_handler);  // Illegal instruction
std::signal(SIGBUS,  sig_handler);  // Bus error

void sig_handler(int s) {
  LG_CRIT("FATAL: Signal %d received (segfault/abort)", s);
  std::fflush(nullptr);  // Flush all logs
  std::_Exit(128 + s);   // Fast exit (no destructors)
}
```

**Thread Crash Policy:**
```cpp
// orchestrator.cpp:410-416 (example for capture thread)
capture_th_ = std::thread([this]() {
  try {
    capture_loop_threadfn();
  } catch (const std::exception& e) {
    LG_CRIT("capture thread crashed: %s", e.what());
    std::abort();  // Bring down entire process
  }
});
```

**Rationale:** Worker thread crashes are unrecoverable. Better to restart entire process than leave system in undefined state.

---

## Performance Characteristics

### Throughput & Latency

**Frame Rate:** 5-10 fps (limited by USB camera on BrightSign XT5)
**Inference Latency:**
- RetinaFace: ~50ms on NPU core 0
- YOLOX: ~80ms on NPU core 1

**End-to-End Latency:**
```
Camera Read       →  50-200ms (USB slow, RTSP ~20ms)
NV12→BGR Convert  →  10ms
Inference (Face)  →  50ms  ┐
Inference (YOLO)  →  80ms  ┴─ Parallel
Tracking          →  5ms
MQTT Publish      →  1ms (async)
────────────────────────────
Total: ~150-300ms (dominated by camera, not NPU)
```

### CPU & NPU Utilization

**NPU Load:** 19% typical (measured on RK3588)
**CPU Load:**
- Capture thread: 15% (mostly blocking I/O)
- Model threads: 5% each (NPU does heavy lifting)
- Supervisor: 2% (mostly sleeping)

**Bottleneck:** Camera capture rate (USB cameras are slow)

### Memory Usage

**Resident Set Size:** ~250 MB
- Model weights: 80 MB (RetinaFace 40MB + YOLOX 40MB)
- Frame buffers: 50 MB (~10 frames × 640×640×3 bytes)
- OpenCV: 40 MB
- GStreamer: 30 MB
- Application: 50 MB

**Peak Allocation:** ~300 MB (includes RKNN internal buffers)

---

## Code Modification Guide

### Adding a New Model

1. **Create Model Runner:**
   ```cpp
   // include/models/model_runner_yourmodel.h
   class YourModelRunner : public IModelRunner {
     bool load(const ModelSpec& spec) noexcept override;
     bool infer(const FrameView& in, InferenceOutputs& out) noexcept override;
     // ...
   };
   ```

2. **Update Model Factory:**
   ```cpp
   // src/models/model_factory.cpp
   if (spec.family == ModelFamily::YourModel) {
     return std::make_unique<YourModelRunner>();
   }
   ```

3. **Add Worker Thread:**
   ```cpp
   // orchestrator.h
   std::thread yourmodel_th_;
   std::atomic<bool> stop_yourmodel_{false};
   FrameMailbox mb_yourmodel_;

   // orchestrator.cpp
   void yourmodel_loop_threadfn() noexcept {
     while (!stop_yourmodel_.load()) {
       auto frame = mb_yourmodel_.takeFrame();
       if (!frame) { sleep(5ms); continue; }
       // Run inference, update fusion state
     }
   }
   ```

4. **Update Supervisor:**
   ```cpp
   // supervisor_loop()
   // Read yourmodel results from fusion_
   // Incorporate into analytics
   ```

### Changing Inference Frame Rate

**Option 1: Skip Frames in Worker**
```cpp
// orchestrator.cpp:1333 (face_loop_threadfn example)
inference_worker::WorkerConfig config{};
config.skip_frames = 2;  // Process every 3rd frame (0=no skip)
```

**Option 2: Throttle Capture**
```cpp
// capture_loop_threadfn()
while (!stop_capture_.load()) {
  if (frame_seq_ % 3 != 0) {  // Publish every 3rd frame
    continue;
  }
  // ... normal capture logic
}
```

### Adding a New Publisher

1. **Implement Interface:**
   ```cpp
   // src/output/my_publisher.cpp
   class MyPublisher : public IPublisher {
     bool start() noexcept override {
       // Connect to service
       return true;
     }

     void publish_result(const PipelineResult& result) noexcept override {
       // Serialize and send
     }

     void stop() noexcept override {
       // Disconnect
     }
   };
   ```

2. **Register in Factory:**
   ```cpp
   // src/output/publisher_factory.cpp
   if (cfg.type == "my_protocol") {
     return std::make_unique<MyPublisher>(cfg);
   }
   ```

3. **Configure in JSON:**
   ```json
   {
     "publishers": [
       {
         "enabled": true,
         "type": "my_protocol",
         "endpoint": "ws://server:8080"
       }
     ]
   }
   ```

### Debugging Threading Issues

**Enable Thread Sanitizer:**
```cmake
# CMakeLists.txt
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -g")
```

**Add Debug Logging:**
```cpp
#define LOG_THREAD_ENTRY() \
  LG_DEBUG("[TID:%lu] %s ENTER", std::this_thread::get_id(), __func__)

#define LOG_THREAD_EXIT() \
  LG_DEBUG("[TID:%lu] %s EXIT", std::this_thread::get_id(), __func__)
```

**Lock Order Rules:**
1. NEVER hold `fusion_.m` while calling external APIs (MQTT, file I/O)
2. NEVER acquire multiple mutexes (only one mutex in this design)
3. Atomic operations do NOT require locks

**Deadlock Prevention:**
- Use `std::lock_guard` (RAII, automatic unlock)
- Keep critical sections SHORT (<1ms ideal)
- Prefer lock-free data structures (mailboxes, atomics)

### Testing Strategy

**Unit Tests:** (not currently implemented, recommended)
- Mock `IInputSource` for deterministic frame injection
- Mock `IModelRunner` for inference bypassing
- Test tracker logic with synthetic detections

**Integration Tests:**
```bash
# File input for reproducibility
./attention_demo --input test.mp4

# Check frame output
ls -lh /tmp/*.jpg

# Verify MQTT messages
mosquitto_sub -t 'brightsign/v3/#' -v
```

**Stress Tests:**
```bash
# Camera disconnect simulation
while true; do
  ./attention_demo --input /dev/video1 &
  PID=$!
  sleep 30
  kill -9 $PID  # Simulate crash
  sleep 5
done
```

---

## Appendix: Key Files Reference

### Critical Implementation Files (AI Modification Priority)

| File | Lines | Purpose | Modification Risk |
|------|-------|---------|------------------|
| `src/orchestration/orchestrator.cpp` | 1415 | Main threading logic, recovery | HIGH |
| `include/pipeline/frame_mailbox.h` | 54 | Lock-free frame fanout | MEDIUM |
| `src/tracking/tracker.cpp` | ~1200 | Person tracking algorithm | MEDIUM |
| `src/input/input_rtsp.cpp` | ~800 | GStreamer RTSP capture | HIGH |
| `src/models/model_runner_retinaface.cpp` | ~600 | Face detection inference | LOW |
| `src/models/model_runner_yolox.cpp` | ~700 | Object detection inference | LOW |
| `src/main.cpp` | 389 | Entry point, config parsing | LOW |

### Configuration Files

- `configs/argus-config.json` - Runtime configuration (input sources, models, publishers)
- `CMakeLists.txt` - Build system, dependencies, cross-compilation
- `Makefile` - Build orchestration wrapper

### Build Artifacts

- `build/attention_demo` - Main executable (aarch64)
- `model/*.rknn` - Compiled NPU models (RetinaFace, YOLOX)
- `/storage/sd/logs/gaze.log` - Runtime logs (rotating, 5MB × 5 files)

---

## Glossary

- **RKNN**: Rockchip Neural Network SDK (NPU inference API)
- **NPU**: Neural Processing Unit (hardware accelerator for ML inference)
- **RGA**: Rockchip Graphics Accelerator (2D image operations)
- **V4L2**: Video4Linux2 (Linux kernel video capture API)
- **NV12**: YUV 4:2:0 planar format (Y plane + interleaved UV)
- **EMA**: Exponential Moving Average (smoothing filter)
- **IoU**: Intersection over Union (bounding box overlap metric)
- **NMS**: Non-Maximum Suppression (removes duplicate detections)
- **SPSC**: Single-Producer Single-Consumer (lock-free queue pattern)
- **GUID**: Globally Unique Identifier (track IDs are local, not truly global)

---

## Version History

- **v1.0** (2026-01-06): Initial documentation covering full system architecture

---

**END OF DOCUMENT**
