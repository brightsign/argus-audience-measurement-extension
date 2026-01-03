#pragma once
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <string>
#include <memory>

// Forward declare Detection - full definition in models/model_runner.h
struct Detection;

// Forward declare ByteTrack components (in bytetrack namespace)
namespace bytetrack {
  class ByteTrackLite;
  struct ByteTrackLiteCfg;
}


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
  // gaze
  bool   has_gaze{false};         // Whether gaze detection is available for this track
  bool   is_gazing{false};        // True if person is looking at camera
  double gaze_time{0.0};          // Accumulated time gazing at camera (seconds)
  float  face_bbox_x0{0.f};       // Face bounding box (if detected)
  float  face_bbox_y0{0.f};
  float  face_bbox_x1{0.f};
  float  face_bbox_y1{0.f};
  // book-keeping
  bool   just_entered{false};
  bool   just_exited{false};
  int    age_frames{0};           // how many frames the track has existed
  int    hits{0};                 // consecutive matched frames
  int    missed{0};               // consecutive frames not matched
};

struct TrackerConfig {
  // Tracker core algorithm selection
  std::string tracker_core{"legacy"}; // "legacy" or "byte" (ByteTrack)

  // === ByteTrack-specific parameters ===
  float byte_det_high{0.50f};        // High confidence threshold for ByteTrack
  float byte_det_low{0.15f};         // Low confidence threshold for ByteTrack
  float byte_match_iou_high{0.70f};  // IoU threshold for high-conf matching
  float byte_match_iou_low{0.50f};   // IoU threshold for low-conf matching
  int   byte_max_age{30};            // Frames to keep unmatched ByteTrack tracks
  int   byte_n_init{2};              // LOWERED: 3→2 for faster face track confirmation (small/distant faces)
  
  // === Legacy EMA/IoU parameters ===
  // Association & lifecycle
  float iou_match_thresh{0.45f};  // V6.2.3: Raised from 0.35 to reduce wrong matches
  int   confirm_hits{2};          // LOWERED: 3→2 for faster track confirmation (weak faces, 320×320 model limitation)
  int   max_missed{30};           // Misses before deletion (~1.0s @30fps)

  // Detection quality
  float min_det_score{0.50f};
  int   min_area_px{1600};

  // Motion & smoothing (V7.1f: per-frame epsilon scaled for 480p-720p slow walks)
  float motion_eps_px{0.25f};     // V7.1f: lowered from 0.8 (absolute per-frame threshold)
  float motion_eps_frac{0.0015f}; // V7.1f: relative per-frame threshold (0.15% of min side)
  float motion_deadband_frac_diag{0.0005f}; // V7.1d: ~0.05% of diagonal (VGA:~0.4px, 720p:~0.7px, 1080p:~1.1px)
  float motion_deadband_px{0.3f}; // V7.1d: frame-to-frame threshold (0.3 px minimum movement)
  float smooth_pos_alpha{0.45f};  // V7.0: slightly smoother bbox tracking
  float smooth_vel_alpha{0.70f};  // V7.1: more responsive (0.65→0.70) for slow lateral motion
  float dir_hysteresis_deg{22.5f};
  float bin_hysteresis_deg{10.0f}; // V7.1f: less sticky (12→10) for faster direction changes

  // Person class
  int   class_person{0};

  // NEW: robust association & publishing
  float max_center_dist_px{80.f};   // distance fallback when IoU small
  float min_speed_px_s{8.0f};       // V7.1e: absolute floor for tiny scenes (good for 480p)
  float min_speed_frac{0.012f};     // V7.1e: additional floor as fraction of min(frame_w,frame_h)
  float max_speed_px_s{90.0f};      // V7.0: hard clamp (120→90 to avoid plateau ceiling)
  int   publish_grace_missed{15};   // 500ms grace period for occlusion bridging
  float enter_exit_border_frac{0.10f}; // ROI border fraction (0.10 => 10% inset)
  
  // V7.1d: direction stability with faster ramp-up and gentler decay
  double dir_hold_ms{850.0};        // V7.1c: base hold time (scaled by resolution/fps)
  float dir_decay_per_s{1.0f};      // V7.0: confidence decay rate when slow
  float dir_reset_conf_thresh{0.35f}; // V7.1d: only reset to "?" if conf drops below this (was 0.40)
  float dir_conf_ramp_up{0.12f};    // V7.1d: faster confidence ramp during motion (was implicit)
  float low_score_thresh{0.90f};    // V7.0: stricter quality gate for direction updates
  
  // V7.1c: asymmetric hysteresis for bin changes (stickier to keep than to change)
  float dir_keep_thresh_deg{8.0f};    // V7.1c: threshold to keep current bin
  float dir_change_thresh_deg{16.0f}; // V7.1c: threshold to accept new bin
  
  // V7.1: acceleration limits (tighter for slow motion)
  float accel_cap_px_s2{400.0f};    // V7.1: tighter cap (450→400) to reduce direction flicker
  
  // Publish filtering (prevent edge/ghost track noise)
  float publish_score_min{0.70f};   // minimum score to publish motion
  float publish_min_area_frac{0.02f}; // minimum area as fraction of frame (2%)
  float publish_border_frac{0.03f}; // border margin to reject edge tracks (3%)
  int   moving_streak_req{2};       // V7.1: faster motion classification (3→2 frames)
};

class Tracker {
public:
  explicit Tracker(const TrackerConfig& cfg = {});
  ~Tracker();

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
  
  // Get current tracker core mode
  const std::string& get_tracker_core() const noexcept { return cfg_.tracker_core; }

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
    
    // V7.1b: velocity history ring buffer for stable direction (5-frame windowed average)
    static constexpr int VEL_HIST_SIZE = 5;
    float vx_hist[VEL_HIST_SIZE]{};
    float vy_hist[VEL_HIST_SIZE]{};
    int   vel_hist_idx{0};
    int   vel_hist_filled{0};  // tracks buffer warmup (0..VEL_HIST_SIZE)
    
    // V6.2: direction confidence and hold
    double last_dir_ts{0.0};      // timestamp of last confident direction
    double last_moving_ts{0.0};   // V7.1b: timestamp of last above-floor movement (for hold logic)
    float  dir_conf{0.0f};        // direction confidence (0..1)
    int    moving_streak{0};      // consecutive frames above speed floor (motion debounce)
    
    // deadband - center history for jitter suppression (V6.2.3: Fixed ring buffer)
    static constexpr int HIST_SIZE = 8;   // ~0.27s @ 30fps for stable motion window
    float cx_hist[HIST_SIZE]{};
    float cy_hist[HIST_SIZE]{};
    float w_hist[HIST_SIZE]{};    // width history for jump detection
    float h_hist[HIST_SIZE]{};    // height history for jump detection
    float area_hist[HIST_SIZE]{}; // V7.0: area history for size stability check
    float x0_hist[HIST_SIZE]{};   // corner history for IoU-based jitter filter
    float y0_hist[HIST_SIZE]{};
    float x1_hist[HIST_SIZE]{};
    float y1_hist[HIST_SIZE]{};
    double ts_hist[HIST_SIZE]{};  // timestamps for real velocity calculation
    int   hist_idx{0};
    int   filled{0};              // tracks buffer warmup (0..HIST_SIZE)
    
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
  
  // ByteTrack integration
  void     update_with_bytetrack(const std::vector<Detection>& dets, double ts, double fps);
  void     update_behavior_fields(TrackStateInternal& t, double ts, double fps);
  bool     is_clean_for_publish(const TrackStateInternal& t) const noexcept;  // Filter edge/ghost tracks

  // ROI and frame info
  int frame_w_{640};
  int frame_h_{480};

  TrackerConfig cfg_;
  std::unordered_map<int, TrackStateInternal> tracks_;
  int next_id_{1};
  
  // ByteTrack instance (only created if tracker_core == "byte")
  std::unique_ptr<bytetrack::ByteTrackLite> byte_tracker_;
};
