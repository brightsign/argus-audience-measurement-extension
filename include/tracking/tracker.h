#pragma once
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <string>

// Forward declare Detection - full definition in models/model_runner.h
struct Detection;

// Track life-cycle states
enum class TrackState : uint8_t { 
  Tentative,   // New track, needs confirmation
  Confirmed,   // Reliable track with multiple matches
  Deleted      // Marked for removal
};

struct TrackedBox {
  int   id;                       // stable GUID (int)
  TrackState state;               // Track state
  float x0, y0, x1, y1;           // smoothed bbox
  float score;                    // latest detection score
  // motion
  float vx{0.f}, vy{0.f};         // px/sec (smoothed velocity of center)
  float speed{0.f};               // magnitude px/sec
  float dir_deg{0.f};             // 0=right, 90=up, 180=left, 270=down
  const char* dir_label;          // "R,UR,U,UL,L,DL,D,DR,?" (quantized or stationary)
  float dir_conf{0.0f};           // direction confidence (0..1) - V6.2
  // time
  double first_ts{0.0};           // seconds
  double last_ts{0.0};            // seconds
  double dwell_s{0.0};            // accumulated visible time
  // book-keeping
  bool   just_entered{false};
  bool   just_exited{false};
  int    age_frames{0};           // how many frames the track has existed
  int    hits{0};                 // consecutive matched frames
  int    missed{0};               // consecutive frames not matched
};

struct TrackerConfig {
  // Association & lifecycle
  float iou_match_thresh{0.45f};  // V6.2.3: Raised from 0.35 to reduce wrong matches
  int   confirm_hits{3};          // Consecutive hits to confirm track
  int   max_missed{12};           // Misses before deletion (~0.4s @30fps)

  // Detection quality
  float min_det_score{0.50f};
  int   min_area_px{1600};

  // Motion & smoothing (V6.2.2: Tuned for 1 Hz publish rate)
  float motion_eps_px{0.8f};      // speed floor (px/frame) paired with fps_for_motion=30
  float motion_deadband_px{2.0f}; // relaxed for 1 Hz updates (was 3.0)
  float smooth_pos_alpha{0.60f};  // reduced from 0.70 for quicker response
  float smooth_vel_alpha{0.55f};  // slightly increased from 0.50 for stability
  float dir_hysteresis_deg{22.5f};
  float bin_hysteresis_deg{8.0f}; // snappier L/R switching (was 12.0)

  // Person class
  int   class_person{0};

  // NEW: robust association & publishing
  float max_center_dist_px{80.f};   // distance fallback when IoU small
  float min_speed_px_s{3.0f};       // reduced from 6.0 for better micro-movement detection
  int   publish_grace_missed{6};    // increased from 4 for better gap bridging
  float enter_exit_border_frac{0.10f}; // ROI border fraction (0.10 => 10% inset)
  
  // V6.2: direction stability
  double dir_hold_ms{300.0};        // keep last direction up to this long when slow
  float dir_decay_per_s{0.5f};      // confidence decay / second when slow
  float low_score_thresh{0.80f};    // detection score threshold for damping direction
};

class Tracker {
public:
  explicit Tracker(const TrackerConfig& cfg = {}) : cfg_(cfg) {}

  // Update tracker with detections at timestamp 'ts' (seconds).
  // Returns current active tracks (copy for convenience).
  std::vector<TrackedBox> update(const std::vector<Detection>& dets, double ts);

  // Access snapshot of active tracks without updating
  std::vector<TrackedBox> active() const;

  // Reset state (e.g., on source restart)
  void reset();

  // NEW: set frame size for ROI enter/exit and distance scaling
  void set_frame_size(int w, int h) noexcept {
    frame_w_ = (w > 0 ? w : 640);
    frame_h_ = (h > 0 ? h : 480);
  }

private:
  struct TrackStateInternal {
    int   id;
    ::TrackState state{::TrackState::Tentative};  // Life-cycle state
    float x0,y0,x1,y1;  // smoothed box
    float score;
    double first_ts, last_ts;
    double dwell_s{0.0};  // Accumulated visible time (only when confirmed & matched)
    int age_frames{0};
    int hits{0};       // Consecutive matched frames
    int missed{0};     // Consecutive unmatched frames
    
    // motion - smoothed positions and size
    float cx{0}, cy{0};           // Current smoothed center
    float w{0}, h{0};             // Smoothed width and height
    float cx_prev{0}, cy_prev{0}; // Previous center for velocity calc
    float vx{0}, vy{0}, speed{0}, dir_deg{0};
    const char* dir_label{"?"};
    int   dir_bin{-1};            // Current direction bin (0-7) for hysteresis
    
    // V6.2: direction confidence and hold
    double last_dir_ts{0.0};      // timestamp of last confident direction
    float  dir_conf{0.0f};        // direction confidence (0..1)
    
    // deadband - center history for jitter suppression (V6.2.2: Reduced for 1 Hz updates)
    static constexpr int HIST_SIZE = 4;  // ~4s @ 1Hz publish (was 6 frames for 30fps)
    float cx_hist[HIST_SIZE]{};
    float cy_hist[HIST_SIZE]{};
    int   hist_idx{0};
    
    // gaze tracking with debouncing
    double gaze_on_ms{0.0};       // Accumulated time looking at camera
    double gaze_off_ms{0.0};      // Accumulated time looking away
    bool   gazing{false};         // Current gaze state (debounced)
    
    // entry/exit tracking
    bool enter_sent{false};  // One-shot flag: true after first enter event
    bool exit_sent{false};   // One-shot flag: true after first exit event
    bool was_inside{false};  // Previous frame inside state for hysteresis
  };

  // helpers
  static float iou(float x0a,float y0a,float x1a,float y1a,
                   float x0b,float y0b,float x1b,float y1b) noexcept;
  static void  center(float x0,float y0,float x1,float y1, float& cx,float& cy) noexcept;
  static const char* dir_label_from_deg(float deg) noexcept;
  static int   bbox_area(float x0,float y0,float x1,float y1) noexcept;

  void     assign_tracks(const std::vector<Detection>& dets, double ts,
                         std::vector<int>& det2trk, std::vector<int>& trk2det);
  void     start_track(const Detection& d, double ts);
  void     update_track(TrackStateInternal& t, const Detection& d, double ts, double fps);
  void     age_and_gc(double ts);

  // ROI and frame info
  int frame_w_{640};
  int frame_h_{480};

  TrackerConfig cfg_;
  std::unordered_map<int, TrackStateInternal> tracks_;
  int next_id_{1};
};

