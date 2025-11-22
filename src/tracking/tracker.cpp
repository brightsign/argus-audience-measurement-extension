#include "tracking/tracker.h"
#include "tracking/byte_tracker_lite.h"
#include "tracking/byte_types.h"
#include "models/model_runner.h"  // For Detection struct definition
#include <algorithm>
#include <cmath>
#include <cstdio>

static inline float clamp01(float v){ return v<0.f?0.f:(v>1.f?1.f:v); }

float Tracker::iou(float x0a,float y0a,float x1a,float y1a,
                   float x0b,float y0b,float x1b,float y1b) noexcept {
  float x0 = std::max(x0a,x0b);
  float y0 = std::max(y0a,y0b);
  float x1 = std::min(x1a,x1b);
  float y1 = std::min(y1a,y1b);
  float iw = std::max(0.f, x1-x0);
  float ih = std::max(0.f, y1-y0);
  float inter = iw*ih;
  float areaA = std::max(0.f,x1a-x0a)*std::max(0.f,y1a-y0a);
  float areaB = std::max(0.f,x1b-x0b)*std::max(0.f,y1b-y0b);
  float uni = areaA + areaB - inter;
  return (uni>0.f)? (inter/uni) : 0.f;
}

void Tracker::center(float x0,float y0,float x1,float y1, float& cx,float& cy) noexcept {
  cx = 0.5f*(x0+x1); cy = 0.5f*(y0+y1);
}

const char* Tracker::dir_label_from_deg(float d) noexcept {
  // 8-way direction: 0°=R, 45°=UR, 90°=U, 135°=UL, 180°=L, 225°=DL, 270°=D, 315°=DR
  // Screen coords: x→right, y→down. We use atan2(-vy, vx) so upward motion is positive angle
  static const char* L[] = {"R","UR","U","UL","L","DL","D","DR"};
  // normalize to [0,360)
  while (d < 0) d += 360.f;
  while (d >=360) d -= 360.f;
  int bin = int(std::round(d / 45.f)) & 7;
  return L[bin];
}

int Tracker::bbox_area(float x0,float y0,float x1,float y1) noexcept {
  int w = int(x1 - x0);
  int h = int(y1 - y0);
  return w * h;
}

void Tracker::assign_tracks(const std::vector<Detection>& dets, double ts,
                            std::vector<int>& det2trk, std::vector<int>& trk2det) {
  (void)ts;
  // Build IoU matrix with distance fallback
  std::vector<int> trk_ids;
  trk_ids.reserve(tracks_.size());
  for (auto& kv : tracks_) trk_ids.push_back(kv.first);

  det2trk.assign(dets.size(), -1);
  trk2det.assign(trk_ids.size(), -1);
  if (dets.empty() || trk_ids.empty()) return;

  struct Edge { int di, ti; float cost; };
  std::vector<Edge> edges;
  edges.reserve(dets.size()*trk_ids.size());

  const float max_cd = cfg_.max_center_dist_px;
  const float max_jump_px = 120.0f;  // V6.2.3: Reject matches with huge center jumps (teleports)
  
  for (int di=0; di<(int)dets.size(); ++di) {
    const auto& d = dets[di];
    float dcx, dcy; 
    center(d.x0, d.y0, d.x1, d.y1, dcx, dcy);

    for (int ti=0; ti<(int)trk_ids.size(); ++ti) {
      const auto& t = tracks_.at(trk_ids[ti]);
      
      // V6.2.3: Check for teleport (prevents speed spikes from wrong associations)
      float dx = dcx - t.cx;
      float dy = dcy - t.cy;
      float dist = std::hypot(dx, dy);
      
      if (dist > max_jump_px) {
        // Reject association - too far to be same track
        continue;
      }

      // Primary: IoU-based matching
      float I = iou(d.x0,d.y0,d.x1,d.y1, t.x0,t.y0,t.x1,t.y1);
      if (I >= cfg_.iou_match_thresh) {
        edges.push_back({di, ti, 1.0f - I}); // lower = better
        continue;
      }

      // Fallback: center distance (handles partial occlusion, aspect changes)
      if (dist <= max_cd) {
        // Map [0..max_cd] -> [0.50..0.99], worse than any IoU match
        float norm = dist / std::max(1e-3f, max_cd);
        float cost = 0.50f + 0.49f * norm;
        edges.push_back({di, ti, cost});
      }
    }
  }

  // Greedy assignment by best match (lowest cost)
  std::sort(edges.begin(), edges.end(),
            [](const Edge& a, const Edge& b){ return a.cost < b.cost; });

  std::vector<char> det_used(dets.size(), 0), trk_used(trk_ids.size(), 0);
  for (const auto& e : edges) {
    if (det_used[e.di] || trk_used[e.ti]) continue;
    det_used[e.di] = 1; trk_used[e.ti] = 1;
    det2trk[e.di] = trk_ids[e.ti];
    trk2det[e.ti] = e.di;
  }
}

void Tracker::start_track(const Detection& d, double ts) {
  TrackStateInternal t;
  t.id = next_id_++;
  t.state = ::TrackState::Tentative;  // Start as tentative
  t.x0=d.x0; t.y0=d.y0; t.x1=d.x1; t.y1=d.y1; t.score=d.score;
  t.first_ts = t.last_ts = ts;
  t.dwell_s = 0.0;
  t.hits = 1;  // First detection counts as a hit
  t.missed = 0;
  
  // Initialize smoothed center and size
  float cx,cy; 
  center(d.x0,d.y0,d.x1,d.y1, cx,cy);
  t.cx = cx;
  t.cy = cy;
  t.w = d.x1 - d.x0;
  t.h = d.y1 - d.y0;
  t.cx_prev = cx; 
  t.cy_prev = cy;
  
  // Initialize history buffer
  for (int i = 0; i < t.HIST_SIZE; ++i) {
    t.cx_hist[i] = cx;
    t.cy_hist[i] = cy;
    t.area_hist[i] = t.w * t.h;  // V7.0: initialize area history
  }
  t.hist_idx = 0;
  
  // Motion state
  t.vx = 0.f;
  t.vy = 0.f;
  t.speed = 0.f;
  t.dir_deg = 0.f;
  t.dir_label = "?";
  t.dir_bin = -1;
  
  // Gaze state
  t.gaze_on_ms = 0.0;
  t.gaze_off_ms = 0.0;
  t.gazing = false;
  
  // Entry/exit tracking
  t.enter_sent = false;
  t.exit_sent = false;
  t.was_inside = false;  // Will be set on first update
  
  tracks_.emplace(t.id, t);
}

void Tracker::update_track(TrackStateInternal& t, const Detection& d, double ts, double fps) {
  // Smooth bbox corners first (EMA)
  float A = clamp01(cfg_.smooth_pos_alpha);
  t.x0 = A*d.x0 + (1.f-A)*t.x0;
  t.y0 = A*d.y0 + (1.f-A)*t.y0;
  t.x1 = A*d.x1 + (1.f-A)*t.x1;
  t.y1 = A*d.y1 + (1.f-A)*t.y1;
  t.score = d.score;

  // Derive center/size from the SMOOTHED corners (not raw detection)
  // This eliminates re-introducing jitter from double-smoothing
  center(t.x0, t.y0, t.x1, t.y1, t.cx, t.cy);
  t.w = t.x1 - t.x0;
  t.h = t.y1 - t.y0;

  // === IMPROVED RING BUFFER MOTION DETECTION ===
  // Parameters
  const int   LAG = 5;                         // velocity window (~5 frames)
  const int   HIST = t.HIST_SIZE;              // e.g. 8

  // 1) Choose the sample to compare against with filled check
  // V7.0: Use actual window length (used) not LAG when buffer not full
  int used = std::min(t.filled, LAG);
  int read_idx = (t.hist_idx + HIST - used) % HIST;

  // Need at least 2 frames of history for velocity
  if (used < 2) {
    // Not enough history yet -> treat as stationary
    t.dir_label = "?";
    t.dir_conf = 0.f;
    t.dir_deg = 0.f;
    t.speed = 0.f;
    
    // Write to history & exit
    t.x0_hist[t.hist_idx] = t.x0;  
    t.y0_hist[t.hist_idx] = t.y0;
    t.x1_hist[t.hist_idx] = t.x1;  
    t.y1_hist[t.hist_idx] = t.y1;
    t.cx_hist[t.hist_idx] = t.cx;  
    t.cy_hist[t.hist_idx] = t.cy;
    t.w_hist[t.hist_idx] = t.w;   
    t.h_hist[t.hist_idx] = t.h;
    t.area_hist[t.hist_idx] = t.w * t.h;  // Keep area history consistent during warmup
    t.ts_hist[t.hist_idx] = ts;
    t.hist_idx = (t.hist_idx + 1) % HIST;
    t.filled = std::min(t.filled + 1, HIST);
    
    t.cx_prev = t.cx;
    t.cy_prev = t.cy;
    t.last_ts = ts;
    t.age_frames++;
    t.hits++;
    t.missed = 0;
    
    if (t.state == ::TrackState::Tentative && t.hits >= cfg_.confirm_hits) {
      t.state = ::TrackState::Confirmed;
    }
    return;
  }

  // 2) Read BEFORE writing the new sample
  float prev_cx = t.cx_hist[read_idx];
  float prev_cy = t.cy_hist[read_idx];
  float prev_w = t.w_hist[read_idx];
  float prev_h = t.h_hist[read_idx];

  // 3) Compute deltas over the lag window
  float dx_hist = t.cx - prev_cx;
  float dy_hist = t.cy - prev_cy;

  // 3.1) ROI enter/exit tracking
  auto inside_roi = [&](float cx, float cy) -> bool {
    const float bx = cfg_.enter_exit_border_frac * frame_w_;
    const float by = cfg_.enter_exit_border_frac * frame_h_;
    return (cx >= bx && cx <= (frame_w_ - bx) &&
            cy >= by && cy <= (frame_h_ - by));
  };
  bool now_inside = inside_roi(t.cx, t.cy);
  
  // Enter/exit one-shots (on state change)
  if (!t.was_inside && now_inside && !t.enter_sent) {
    t.enter_sent = true;
  }
  if (t.was_inside && !now_inside && !t.exit_sent) {
    t.exit_sent = true;
  }

  // ==== V7.0: ENHANCED STATIONARY GATE with center+size stability ====
  // Use adaptive deadband based on box size
  float diag = std::hypot(t.w, t.h);
  const float adaptive_k = 0.012f;  // 1.2% of diagonal
  // Safer clamp (handles configs where motion_deadband_px < 8.0f)
  float max_db_cap = std::max(cfg_.motion_deadband_px, 8.0f);
  float adaptive_db = std::min(max_db_cap, adaptive_k * diag);
  
  // Center drift over history window
  int oldest = (t.hist_idx + HIST - used) % HIST;
  float dx_hist_full = t.cx - t.cx_hist[oldest];
  float dy_hist_full = t.cy - t.cy_hist[oldest];
  float disp = std::hypot(dx_hist_full, dy_hist_full);
  
  // Area stability (ignore breathing of the box)
  float area_now = t.w * t.h;
  float area_prev = t.area_hist[oldest];
  const float area_tol = 0.06f;  // 6% area jitter tolerated
  float area_change = (area_prev > 1.f) ? std::fabs(area_now - area_prev)/area_prev : 0.f;
  bool size_stable = (area_change < area_tol);
  bool center_stable = (disp < adaptive_db);
  
  // If both center and size stable -> treat as stationary
  if (center_stable && size_stable) {
    // Stationary: zero out motion and reset debounce
    t.vx = 0.f;
    t.vy = 0.f;
    t.speed = 0.f;
    t.dir_label = "?";
    t.dir_deg = 0.f;
    t.dir_conf = 0.f;
    t.moving_streak = 0;  // Reset debounce counter
    
    // Write to history & exit
    t.x0_hist[t.hist_idx] = t.x0;  
    t.y0_hist[t.hist_idx] = t.y0;
    t.x1_hist[t.hist_idx] = t.x1;  
    t.y1_hist[t.hist_idx] = t.y1;
    t.cx_hist[t.hist_idx] = t.cx;  
    t.cy_hist[t.hist_idx] = t.cy;
    t.w_hist[t.hist_idx] = t.w;   
    t.h_hist[t.hist_idx] = t.h;
    t.area_hist[t.hist_idx] = area_now;
    t.ts_hist[t.hist_idx] = ts;
    t.hist_idx = (t.hist_idx + 1) % HIST;
    t.filled = std::min(t.filled + 1, HIST);
    
    t.cx_prev = t.cx;
    t.cy_prev = t.cy;
    
    double time_delta = ts - t.last_ts;
    t.last_ts = ts;
    if (t.state == ::TrackState::Confirmed && now_inside) {
      t.dwell_s += time_delta;
    }
    t.was_inside = now_inside;
    
    t.age_frames++;
    t.hits++;
    t.missed = 0;
    
    if (t.state == ::TrackState::Tentative && t.hits >= cfg_.confirm_hits) {
      t.state = ::TrackState::Confirmed;
    }
    return;  // Early exit - stationary
  }

  // 3.5) Legacy Zero-motion gate with reframe veto (fallback for edge cases)
  auto iou_calc = [&](float x0,float y0,float x1,float y1,
                      float u0,float v0,float u1,float v1)->float {
    float ix0 = std::max(x0,u0), iy0 = std::max(y0,v0);
    float ix1 = std::min(x1,u1), iy1 = std::min(y1,v1);
    float iw = std::max(0.f, ix1-ix0), ih = std::max(0.f, iy1-iy0);
    float inter = iw*ih;
    float a = (x1-x0)*(y1-y0);
    float b = (u1-u0)*(v1-v0);
    return inter / std::max(1.f, a + b - inter);
  };

  float iou_now = iou_calc(t.x0, t.y0, t.x1, t.y1,
                           t.x0_hist[read_idx], t.y0_hist[read_idx],
                           t.x1_hist[read_idx], t.y1_hist[read_idx]);
  
  float center_shift = std::hypot(dx_hist, dy_hist);
  float dsize = std::fabs((t.w * t.h) - (prev_w * prev_h)) / std::max(1.f, prev_w * prev_h);

  // Zero-motion gate: box essentially unchanged
  float db_px = std::max(4.f, 0.015f * diag);  // 1.5% diag
  bool tiny_center_shift = (center_shift < db_px);
  bool tiny_size_change  = (dsize < 0.03f);    // <3% area change
  bool boxes_almost_same = (iou_now > 0.95f);  // 95% overlap

  // Reframe veto: detector resized/reframed but center stable (aspect jump)
  float center_eps = 0.02f * diag;  // 2% diag
  bool reframe_veto = (iou_now < 0.30f && center_shift < center_eps);

  if (tiny_center_shift && (tiny_size_change || boxes_almost_same)) {
    // Treat as stationary - box is essentially unchanged
    t.dir_label = "?";
    t.dir_deg   = 0.f;
    t.dir_conf  = 0.f;
    t.speed     = 0.f;
    t.moving_streak = 0;  // Reset debounce counter
    
    // Write to history & exit
    t.x0_hist[t.hist_idx] = t.x0;  
    t.y0_hist[t.hist_idx] = t.y0;
    t.x1_hist[t.hist_idx] = t.x1;  
    t.y1_hist[t.hist_idx] = t.y1;
    t.cx_hist[t.hist_idx] = t.cx;  
    t.cy_hist[t.hist_idx] = t.cy;
    t.w_hist[t.hist_idx] = t.w;   
    t.h_hist[t.hist_idx] = t.h;
    t.area_hist[t.hist_idx] = area_now;
    t.ts_hist[t.hist_idx] = ts;
    t.hist_idx = (t.hist_idx + 1) % HIST;
    t.filled = std::min(t.filled + 1, HIST);

    t.cx_prev = t.cx;
    t.cy_prev = t.cy;
    
    double time_delta = ts - t.last_ts;
    t.last_ts = ts;
    if (t.state == ::TrackState::Confirmed && now_inside) {
      t.dwell_s += time_delta;
    }
    t.was_inside = now_inside;
    
    t.age_frames++;
    t.hits++;
    t.missed = 0;
    
    if (t.state == ::TrackState::Tentative && t.hits >= cfg_.confirm_hits) {
      t.state = ::TrackState::Confirmed;
    }
    return;  // Early exit - stationary
  }

  if (reframe_veto) {
    // Size/aspect jumped but center stable -> veto as stationary
    t.dir_label = "?";
    t.dir_deg   = 0.f;
    t.dir_conf  = 0.f;
    t.speed     = 0.f;
    t.moving_streak = 0;  // Reset debounce counter
    
    // Write to history & exit
    t.x0_hist[t.hist_idx] = t.x0;  
    t.y0_hist[t.hist_idx] = t.y0;
    t.x1_hist[t.hist_idx] = t.x1;  
    t.y1_hist[t.hist_idx] = t.y1;
    t.cx_hist[t.hist_idx] = t.cx;  
    t.cy_hist[t.hist_idx] = t.cy;
    t.w_hist[t.hist_idx] = t.w;   
    t.h_hist[t.hist_idx] = t.h;
    t.area_hist[t.hist_idx] = area_now;
    t.ts_hist[t.hist_idx] = ts;
    t.hist_idx = (t.hist_idx + 1) % HIST;
    t.filled = std::min(t.filled + 1, HIST);

    t.cx_prev = t.cx;
    t.cy_prev = t.cy;
    
    double time_delta = ts - t.last_ts;
    t.last_ts = ts;
    if (t.state == ::TrackState::Confirmed && now_inside) {
      t.dwell_s += time_delta;
    }
    t.was_inside = now_inside;
    
    t.age_frames++;
    t.hits++;
    t.missed = 0;
    
    if (t.state == ::TrackState::Tentative && t.hits >= cfg_.confirm_hits) {
      t.state = ::TrackState::Confirmed;
    }
    return;  // Early exit - reframe veto
  }

  // 4) Compute velocity over actual elapsed time
  // V7.0: Use real timestamps from history, not FPS estimate
  double dt_hist = std::max(1e-3, ts - t.ts_hist[read_idx]);

  float inst_vx = float((t.cx - prev_cx) / dt_hist);
  float inst_vy = float((t.cy - prev_cy) / dt_hist);

  // 5) EMA on velocity
  float B = clamp01(cfg_.smooth_vel_alpha);
  t.vx = B * inst_vx + (1.f - B) * t.vx;
  t.vy = B * inst_vy + (1.f - B) * t.vy;

  // 6) Speed with V7.0 enhanced floor and acceleration cap
  float sp = std::hypot(t.vx, t.vy);
  
  // V7.0: Velocity floor - very small speeds → treat as stationary
  if (sp < cfg_.min_speed_px_s) {
    double dt_dir = std::max(0.0, ts - t.last_dir_ts);
    // Only keep prior direction during hold if we had a recent confirmed motion streak.
    // This prevents showing directions when you're basically idle.
    if (dt_dir * 1000.0 <= cfg_.dir_hold_ms &&
        t.dir_conf > 0.15f &&
        t.moving_streak >= cfg_.moving_streak_req) {
      // Keep previous direction during hold period, decay confidence
      t.dir_conf = std::max(0.0f, t.dir_conf - float(cfg_.dir_decay_per_s * dt_dir));
      t.speed = sp;    // Show small number during hold
    } else {
      // Too long slow / no confidence → unknown
      t.dir_label = "?";
      t.dir_deg = 0.f;
      t.dir_conf = 0.f;
      t.speed = 0.f;
      t.moving_streak = 0;  // Reset debounce counter
    }
    
    // Write to history & finalize
    t.x0_hist[t.hist_idx] = t.x0;
    t.y0_hist[t.hist_idx] = t.y0;
    t.x1_hist[t.hist_idx] = t.x1;
    t.y1_hist[t.hist_idx] = t.y1;
    t.cx_hist[t.hist_idx] = t.cx;
    t.cy_hist[t.hist_idx] = t.cy;
    t.w_hist[t.hist_idx] = t.w;
    t.h_hist[t.hist_idx] = t.h;
    t.area_hist[t.hist_idx] = area_now;
    t.ts_hist[t.hist_idx] = ts;
    t.hist_idx = (t.hist_idx + 1) % HIST;
    t.filled = std::min(t.filled + 1, HIST);

    t.cx_prev = t.cx;
    t.cy_prev = t.cy;
    
    double time_delta = ts - t.last_ts;
    t.last_ts = ts;
    if (t.state == ::TrackState::Confirmed && now_inside) {
      t.dwell_s += time_delta;
    }
    t.was_inside = now_inside;
    
    t.age_frames++;
    t.hits++;
    t.missed = 0;
    
    if (t.state == ::TrackState::Tentative && t.hits >= cfg_.confirm_hits) {
      t.state = ::TrackState::Confirmed;
    }
    return;  // Exit - below speed floor
  }

  // V7.0: Acceleration cap (prevents single-frame leaps)
  double dt = std::max(1e-3, ts - t.last_ts);
  float sp_capped = std::min(sp, t.speed + cfg_.accel_cap_px_s2 * float(dt));
  t.speed = std::min(sp_capped, cfg_.max_speed_px_s);
  
  // We're moving: bump debounce counter
  t.moving_streak = std::min(t.moving_streak + 1, 1000);

  // 7) Moving: compute direction with smart hysteresis
  float deg = std::atan2(-t.vy, t.vx) * 180.f / float(M_PI);
  if (deg < 0.f) deg += 360.f;

  // Direction bin calculation with edge avoidance
  float dir_center = (t.dir_bin >= 0) ? t.dir_bin * 45.f : deg;
  float dir_delta = deg - dir_center;
  while (dir_delta > 180.f) dir_delta -= 360.f;
  while (dir_delta < -180.f) dir_delta += 360.f;
  
  // Only update bin if we're significantly away from current bin center
  if (t.dir_bin == -1 || std::fabs(dir_delta) > (22.5f + cfg_.bin_hysteresis_deg)) {
    t.dir_bin = static_cast<int>(std::round(deg / 45.f)) & 7;
  }

  t.dir_label = dir_label_from_deg(t.dir_bin * 45.f);
  t.dir_deg = deg;

  // Compute direction confidence - penalizes edge proximity
  float ang_err = deg - (t.dir_bin * 45.f);
  while (ang_err > 180.f) ang_err -= 360.f;
  while (ang_err < -180.f) ang_err += 360.f;
  ang_err = std::fabs(ang_err);
  float center_score = std::max(0.f, 1.f - (ang_err / 45.f));
  float sp_rel = std::min(1.f, sp / (cfg_.min_speed_px_s * 3.f));
  t.dir_conf = 0.5f * sp_rel + 0.5f * center_score;
  
  // Soft gate: require a short moving streak before publishing a direction
  if (t.moving_streak < cfg_.moving_streak_req) {
    t.dir_label = "?";
    t.dir_deg   = 0.f;
    t.dir_conf  = 0.f;
  }
  
  t.last_dir_ts = ts;

  // 7) NOW write the current sample and advance the ring
  t.x0_hist[t.hist_idx] = t.x0;
  t.y0_hist[t.hist_idx] = t.y0;
  t.x1_hist[t.hist_idx] = t.x1;
  t.y1_hist[t.hist_idx] = t.y1;
  t.cx_hist[t.hist_idx] = t.cx;
  t.cy_hist[t.hist_idx] = t.cy;
  t.w_hist[t.hist_idx] = t.w;
  t.h_hist[t.hist_idx] = t.h;
  t.area_hist[t.hist_idx] = t.w * t.h;  // V7.0: track area history
  t.ts_hist[t.hist_idx] = ts;
  t.hist_idx = (t.hist_idx + 1) % HIST;
  t.filled = std::min(t.filled + 1, HIST);

  // Update previous center for compatibility
  t.cx_prev = t.cx;
  t.cy_prev = t.cy;
  
  // Update time and dwell (only when confirmed & inside ROI)
  double time_delta = ts - t.last_ts;
  t.last_ts = ts;
  if (t.state == ::TrackState::Confirmed && now_inside) {
    t.dwell_s += time_delta;  // Accumulate time inside ROI only
  }
  t.was_inside = now_inside;
  
  t.age_frames++;
  t.hits++;      // Consecutive matched frames
  t.missed = 0;  // Reset miss counter on match
  
  // Promote tentative to confirmed after enough hits
  if (t.state == ::TrackState::Tentative && t.hits >= cfg_.confirm_hits) {
    t.state = ::TrackState::Confirmed;
  }
}

void Tracker::age_and_gc(double ts) {
  std::vector<int> to_erase;
  for (auto& kv : tracks_) {
    auto& t = kv.second;
    t.missed++;
    t.hits = 0;
    
    // V6.2: Constant-velocity prediction when unmatched (carry motion through gaps)
    double dt = (ts - t.last_ts);
    t.last_ts = ts;
    
    if (dt > 0.0 && dt < 1.0) {  // Sanity check: dt should be ~33ms for 30fps
      // Predict center position using last velocity
      t.cx += t.vx * dt;
      t.cy += t.vy * dt;
      
      // Keep bbox size; update corners from predicted center
      float hw = 0.5f * t.w, hh = 0.5f * t.h;
      t.x0 = t.cx - hw; 
      t.y0 = t.cy - hh;
      t.x1 = t.cx + hw; 
      t.y1 = t.cy + hh;
      
      // Update history buffer with predicted position
      t.cx_hist[t.hist_idx] = t.cx;
      t.cy_hist[t.hist_idx] = t.cy;
      t.hist_idx = (t.hist_idx + 1) % t.HIST_SIZE;
    }
    
    // Decay direction confidence during gaps
    if (dt > 0.0) {
      t.dir_conf = std::max(0.0f, t.dir_conf - float(cfg_.dir_decay_per_s * dt));
      if (t.dir_conf <= 0.15f) {
        t.dir_label = "?";
        t.dir_deg = 0.0f;
        t.speed = 0.f;
      }
    }
    
    // Delete tracks that exceed max missed frames
    if (t.missed > cfg_.max_missed) {
      to_erase.push_back(t.id);
    }
  }
  for (int id : to_erase) tracks_.erase(id);
}

std::vector<TrackedBox> Tracker::update(const std::vector<Detection>& dets_raw, double ts) {
  // Route to ByteTrack or legacy EMA/IoU based on configuration
  if (cfg_.tracker_core == "byte" && byte_tracker_) {
    // Use ByteTrack core
    static double last_ts = 0.0;
    double fps_publish = 30.0;
    if (last_ts > 0 && ts > last_ts) {
      double dt = ts - last_ts;
      fps_publish = 1.0 / std::max(0.001, dt);
      fps_publish = std::min(60.0, std::max(1.0, fps_publish));
    }
    last_ts = ts;
    
    const double fps_for_motion = 30.0;  // Camera FPS
    
    update_with_bytetrack(dets_raw, ts, fps_for_motion);
    
    // Build output
    std::vector<TrackedBox> out;
    out.reserve(tracks_.size());
    for (auto& kv : tracks_) {
      auto& t = kv.second;
      if (t.state != ::TrackState::Confirmed) continue;
      bool fresh = (t.missed <= cfg_.publish_grace_missed);
      if (!fresh) continue;
      
      TrackedBox o;
      o.id = t.id;
      o.state = t.state;
      o.x0=t.x0; o.y0=t.y0; o.x1=t.x1; o.y1=t.y1;
      o.score=t.score;
      o.vx=t.vx; o.vy=t.vy; o.speed=t.speed;
      o.dir_deg=t.dir_deg; o.dir_label=t.dir_label;
      o.dir_conf=t.dir_conf;
      o.first_ts=t.first_ts; o.last_ts=t.last_ts;
      o.dwell_s = t.dwell_s;
      o.age_frames=t.age_frames;
      o.hits=t.hits;
      o.missed=t.missed;
      
      // Apply publish filters to suppress edge/ghost track motion
      bool is_clean = is_clean_for_publish(t);
      bool stale = (t.missed > 0);
      bool low_score = (t.score < cfg_.low_score_thresh);
      
      // Force stationary if track fails cleanliness checks
      if (!is_clean || stale || low_score) {
        o.dir_label = "?";
        o.dir_deg   = 0.0f;
        o.dir_conf  = 0.0f;
        o.speed     = 0.f;
      } else if (o.speed < cfg_.min_speed_px_s) {
        // Also enforce speed floor for clean tracks
        o.dir_label = "?";
        o.dir_deg   = 0.0f;
        o.dir_conf  = 0.0f;
      }
      
      o.just_entered = t.enter_sent;
      o.just_exited = t.exit_sent;
      
      t.enter_sent = false;
      t.exit_sent = false;
      
      out.push_back(o);
    }
    return out;
  }
  
  // Legacy EMA/IoU path
  // V6.2.2: Use camera FPS for motion calculation, not publish rate
  // The publish rate may be ~1 Hz, but motion calculations need actual camera FPS
  static double last_ts = 0.0;
  double fps_publish = 30.0;  // Default
  if (last_ts > 0 && ts > last_ts) {
    double dt = ts - last_ts;
    fps_publish = 1.0 / std::max(0.001, dt);  // Clamp to avoid div by zero
    fps_publish = std::min(60.0, std::max(1.0, fps_publish));  // Reasonable range
  }
  last_ts = ts;
  
  // Force motion calculations to use camera FPS (30), not publish rate (~1 Hz)
  // This prevents motion gates from being too strong when updates are slow
  const double fps_for_motion = 30.0;  // Camera FPS, not publish FPS

  // 1) Filter detections: class, score, and bbox area
  std::vector<Detection> dets;
  dets.reserve(dets_raw.size());
  for (const auto& d : dets_raw) {
    // Check class
    if (cfg_.class_person >= 0 && d.class_id != cfg_.class_person) continue;
    // Check score
    if (d.score < cfg_.min_det_score) continue;
    // Check area (kill tiny boxes)
    int area = bbox_area(d.x0, d.y0, d.x1, d.y1);
    if (area < cfg_.min_area_px) continue;
    dets.push_back(d);
  }

  // 2) Associate detections to existing tracks
  std::vector<int> det2trk, trk2det;
  assign_tracks(dets, ts, det2trk, trk2det);

  // 3) Update matched tracks
  for (int i=0; i<(int)dets.size(); ++i) {
    int trk_id = det2trk[i];
    if (trk_id < 0) continue;
    auto it = tracks_.find(trk_id);
    if (it == tracks_.end()) continue;
    update_track(it->second, dets[i], ts, fps_for_motion);  // V6.2.2: Use camera FPS
  }

  // 4) Handle unmatched tracks (increment missed, check life-cycle)
  for (auto& kv : tracks_) {
    bool matched = false;
    for (int i=0; i<(int)dets.size(); ++i) {
      if (det2trk[i] == kv.first) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      auto& t = kv.second;
      t.missed++;
      t.hits = 0;  // Reset hit streak on miss
      t.last_ts = ts;  // Keep time coherent even when missed
      
      // Life-cycle transitions on miss
      if (t.state == ::TrackState::Tentative && t.missed > 2) {
        // Tentative track never confirmed → delete quickly
        t.state = ::TrackState::Deleted;
      } else if (t.state == ::TrackState::Confirmed && t.missed > cfg_.max_missed) {
        // Confirmed track lost for too long → delete
        t.state = ::TrackState::Deleted;
      }
    }
  }

  // 5) Start new tracks for unmatched detections
  for (int i=0; i<(int)dets.size(); ++i) {
    if (det2trk[i] >= 0) continue;
    start_track(dets[i], ts);
  }

  // 6) Garbage collect deleted tracks
  std::vector<int> removed;
  for (auto it = tracks_.begin(); it != tracks_.end();) {
    if (it->second.state == ::TrackState::Deleted) {
      removed.push_back(it->first);
      it = tracks_.erase(it);
    } else {
      ++it;
    }
  }

  // 7) Build output snapshot - confirmed tracks within publish grace period
  // "Fresh" means: within publish_grace_missed frames to prevent gaps
  std::vector<TrackedBox> out;
  out.reserve(tracks_.size());
  for (auto& kv : tracks_) {
    auto& t = kv.second;
    // Only output confirmed tracks
    if (t.state != ::TrackState::Confirmed) continue;
    
    // Fresh tracks: within publish grace period (handles brief detector gaps)
    bool fresh = (t.missed <= cfg_.publish_grace_missed);
    if (!fresh) continue;
    
    TrackedBox o;
    o.id = t.id;
    o.state = t.state;
    o.x0=t.x0; o.y0=t.y0; o.x1=t.x1; o.y1=t.y1;
    o.score=t.score;
    o.vx=t.vx; o.vy=t.vy; o.speed=t.speed;
    o.dir_deg=t.dir_deg; o.dir_label=t.dir_label;
    o.dir_conf=t.dir_conf;  // V6.2.3: CRITICAL - was missing!
    o.first_ts=t.first_ts; o.last_ts=t.last_ts;
    o.dwell_s = t.dwell_s;  // Accumulated time inside ROI
    o.age_frames=t.age_frames;
    o.hits=t.hits;
    o.missed=t.missed;
    
    // V6.2.3: Enforce stationary guard - only force dir:"?" when below speed threshold
    if (o.speed < cfg_.min_speed_px_s) {
      o.dir_label = "?";
      o.dir_deg   = 0.0f;
      o.dir_conf  = 0.0f;
    }
    
    // Entry/exit one-shot flags
    o.just_entered = t.enter_sent;
    o.just_exited = t.exit_sent;
    
    // Clear one-shot flags after reading
    t.enter_sent = false;
    t.exit_sent = false;
    
    out.push_back(o);
  }
  return out;
}

std::vector<TrackedBox> Tracker::active() const {
  std::vector<TrackedBox> out;
  out.reserve(tracks_.size());
  for (auto& kv : tracks_) {
    const auto& t = kv.second;
    TrackedBox o;
    o.id = t.id; o.x0=t.x0; o.y0=t.y0; o.x1=t.x1; o.y1=t.y1; o.score=t.score;
    o.vx=t.vx; o.vy=t.vy; o.speed=t.speed; o.dir_deg=t.dir_deg; o.dir_label=t.dir_label;
    o.dir_conf=t.dir_conf;  // V6.2: Include direction confidence
    o.first_ts=t.first_ts; o.last_ts=t.last_ts; o.dwell_s=(t.last_ts - t.first_ts);
    o.age_frames=t.age_frames;
    o.missed=t.missed;
    
    // V6.2.3: Enforce consistent "stationary/unknown" state at publish time
    // Only force dir:"?" when truly below speed threshold
    if (o.speed < cfg_.min_speed_px_s) {
      o.dir_label = "?";
      o.dir_deg   = 0.0f;
      o.dir_conf  = 0.0f;   // Fully unknown when below speed floor
    }
    
    out.push_back(o);
  }
  return out;
}

void Tracker::reset() {
  tracks_.clear();
  next_id_ = 1;
}

// Constructor - initialize ByteTrack if configured
Tracker::Tracker(const TrackerConfig& cfg) : cfg_(cfg) {
  if (cfg_.tracker_core == "byte") {
    bytetrack::ByteTrackLiteCfg bt_cfg;
    bt_cfg.det_high = cfg_.byte_det_high;
    bt_cfg.det_low = cfg_.byte_det_low;
    bt_cfg.match_iou_high = cfg_.byte_match_iou_high;
    bt_cfg.match_iou_low = cfg_.byte_match_iou_low;
    bt_cfg.max_age = cfg_.byte_max_age;
    bt_cfg.n_init = cfg_.byte_n_init;
    
    byte_tracker_ = std::make_unique<bytetrack::ByteTrackLite>(bt_cfg);
    fprintf(stderr, "[Tracker] Initialized with ByteTrack core (det_high=%.2f, det_low=%.2f)\n",
            bt_cfg.det_high, bt_cfg.det_low);
  } else {
    fprintf(stderr, "[Tracker] Initialized with Legacy EMA/IoU core\n");
  }
}

// Destructor
Tracker::~Tracker() = default;

// ===== Helper Functions =====

// Check if track is clean enough to publish motion (filters edge/ghost tracks)
bool Tracker::is_clean_for_publish(const TrackStateInternal& t) const noexcept {
  // a) Only confirmed and freshly updated
  if (t.state != ::TrackState::Confirmed) return false;
  if (t.missed > 0) return false;  // stale - not updated this frame
  
  // b) Confidence gate
  if (t.score < cfg_.publish_score_min) return false;
  
  // c) Size gate (reject tiny edge slivers)
  float area = (t.x1 - t.x0) * (t.y1 - t.y0);
  float min_area = cfg_.publish_min_area_frac * frame_w_ * frame_h_;
  if (area < min_area) return false;
  
  // d) Border gate (ignore objects hugging the border)
  const float border = cfg_.publish_border_frac;
  float cx = 0.5f * (t.x0 + t.x1);
  float cy = 0.5f * (t.y0 + t.y1);
  if (cx < border * frame_w_ || cx > (1.f - border) * frame_w_ ||
      cy < border * frame_h_ || cy > (1.f - border) * frame_h_) {
    return false;
  }
  
  return true;
}

// ===== ByteTrack Integration =====

// Update behavior fields (deadband, velocity, direction, ROI, dwell) for a track
// This is the "behavior layer" that works with both legacy and ByteTrack cores
void Tracker::update_behavior_fields(TrackStateInternal& t, double ts, double fps) {
  // Calculate center and size from KF-smoothed bbox (no additional EMA smoothing)
  center(t.x0, t.y0, t.x1, t.y1, t.cx, t.cy);
  t.w = t.x1 - t.x0;
  t.h = t.y1 - t.y0;
  
  // === ROI TRACKING ===
  auto inside_roi = [&](float cx, float cy) -> bool {
    const float bx = cfg_.enter_exit_border_frac * frame_w_;
    const float by = cfg_.enter_exit_border_frac * frame_h_;
    return (cx >= bx && cx <= (frame_w_ - bx) &&
            cy >= by && cy <= (frame_h_ - by));
  };
  bool now_inside = inside_roi(t.cx, t.cy);
  
  if (!t.was_inside && now_inside && !t.enter_sent) {
    t.enter_sent = true;
  }
  if (t.was_inside && !now_inside && !t.exit_sent) {
    t.exit_sent = true;
  }
  
  // === VELOCITY-BASED MOTION DETECTION (uses KF velocity, not history) ===
  // Speed magnitude from KF velocity (px/s) - already smoothed by EMA in caller
  float sp = std::hypot(t.vx, t.vy);
  
  // Motion floor (px/s) - start around 30-40 for 640x480 indoors
  const float speed_floor = std::max(10.0f, cfg_.min_speed_px_s);
  const float max_speed_px_s = 120.0f;  // reduced from 180 to avoid edge track spikes
  
  // Clamp speed to max
  sp = std::min(sp, max_speed_px_s);
  
  // If below motion floor, treat as stationary (jitter suppression)
  if (sp < speed_floor) {
    // Reset motion streak counter
    t.moving_streak = 0;
    
    // Keep sticky direction with decay if configured
    double dt_dir = std::max(0.0, ts - t.last_dir_ts);
    if (dt_dir * 1000.0 <= cfg_.dir_hold_ms && t.dir_conf > 0.15f) {
      // Decay confidence but keep direction
      t.dir_conf = std::max(0.0f, t.dir_conf - float(cfg_.dir_decay_per_s * dt_dir));
      t.speed = 0.f;  // Show as stationary even if direction is sticky
    } else {
      // Too long stationary or no confidence - unknown
      t.dir_label = "?";
      t.dir_deg = 0.0f;
      t.speed = 0.f;
      t.dir_conf = 0.0f;
    }
  } else {
    // Above speed floor - increment motion streak
    t.moving_streak++;
    
    // Require N consecutive frames before confirming motion (debounce 1-2 frame spikes)
    if (t.moving_streak < cfg_.moving_streak_req) {
      // Not stable yet - publish as stationary
      t.dir_label = "?";
      t.dir_deg = 0.0f;
      t.speed = 0.f;
      t.dir_conf = 0.0f;
    } else {
      // Stable motion - compute direction from KF velocity
    // 0° points to +X (right); atan2 uses y-up in math → flip sign to match image coordinates
    float deg = std::atan2(-t.vy, t.vx) * 180.f / float(M_PI);
    if (deg < 0.f) deg += 360.f;

    // Damping when score is low (optional smoothing)
    const float low_score_thresh = 0.80f;  // configurable
    float dir_weight = (t.score < low_score_thresh) ? 0.3f : 1.0f;
    if (dir_weight < 1.0f && t.dir_conf > 0.0f && t.dir_bin >= 0) {
      float prev = t.dir_deg;
      float d = deg - prev;
      while (d > 180.f) d -= 360.f;
      while (d < -180.f) d += 360.f;
      deg = prev + dir_weight * d;
      if (deg < 0.f) deg += 360.f;
      if (deg >= 360.f) deg -= 360.f;
    }
    
    t.dir_deg = deg;
    
    // 8-way binning with hysteresis
    int bin_now = static_cast<int>(std::round(deg / 45.f)) & 7;
    if (t.dir_bin == -1) {
      t.dir_bin = bin_now;
    } else {
      float current_center = t.dir_bin * 45.f;
      float ang_diff = deg - current_center;
      while (ang_diff > 180.f) ang_diff -= 360.f;
      while (ang_diff < -180.f) ang_diff += 360.f;
      if (std::fabs(ang_diff) > (22.5f + cfg_.bin_hysteresis_deg)) {
        t.dir_bin = bin_now;
      }
    }
    t.dir_label = dir_label_from_deg(t.dir_bin * 45.f);
    
    // Confidence: combine speed margin & distance from bin boundary
    float sp_rel = std::min(1.0f, sp / (speed_floor * 3.0f));
    float center = t.dir_bin * 45.f;
    float ang_err = deg - center;
    while (ang_err > 180.f) ang_err -= 360.f;
    while (ang_err < -180.f) ang_err += 360.f;
    ang_err = std::fabs(ang_err);
    float center_score = std::max(0.f, 1.f - (ang_err / 45.f));
    t.dir_conf = std::max(0.0f, std::min(1.0f, 0.5f*sp_rel + 0.5f*center_score));
    
    // Publish speed
    t.speed = sp;
    t.last_dir_ts = ts;
    }  // end moving_streak debounce
  }  // end speed floor check
  
  // Update history buffer (for compatibility, though not used for velocity anymore)
  t.cx_hist[t.hist_idx] = t.cx;
  t.cy_hist[t.hist_idx] = t.cy;
  t.w_hist[t.hist_idx] = t.w;
  t.h_hist[t.hist_idx] = t.h;
  t.area_hist[t.hist_idx] = t.w * t.h;  // V7.0: track area history
  t.x0_hist[t.hist_idx] = t.x0;
  t.y0_hist[t.hist_idx] = t.y0;
  t.x1_hist[t.hist_idx] = t.x1;
  t.y1_hist[t.hist_idx] = t.y1;
  t.ts_hist[t.hist_idx] = ts;
  t.hist_idx = (t.hist_idx + 1) % t.HIST_SIZE;
  t.filled = std::min(t.filled + 1, t.HIST_SIZE);
  
  t.cx_prev = t.cx;
  t.cy_prev = t.cy;
  
  // Update dwell time (only when confirmed & inside ROI)
  double time_delta = ts - t.last_ts;
  t.last_ts = ts;
  if (t.state == ::TrackState::Confirmed && now_inside) {
    t.dwell_s += time_delta;
  }
  t.was_inside = now_inside;
}

// ByteTrack-based update path
void Tracker::update_with_bytetrack(const std::vector<Detection>& dets, double ts, double fps) {
  if (!byte_tracker_) {
    fprintf(stderr, "[Tracker] ERROR: ByteTrack not initialized!\n");
    return;
  }
  
  // Build ByteTrack detections (camera-space, class=0 for person)
  // Use fully qualified bytetrack::Detection to avoid confusion with models/model_runner.h Detection
  std::vector<bytetrack::Detection> bt_dets;
  bt_dets.reserve(dets.size());
  for (const auto& d : dets) {
    if (cfg_.class_person >= 0 && d.class_id != cfg_.class_person) continue;
    if (d.score < cfg_.min_det_score) continue;
    int area = bbox_area(d.x0, d.y0, d.x1, d.y1);
    if (area < cfg_.min_area_px) continue;
    
    bytetrack::Detection bt_det;  // ByteTrack Detection from byte_types.h
    bt_det.box.x0 = d.x0;
    bt_det.box.y0 = d.y0;
    bt_det.box.x1 = d.x1;
    bt_det.box.y1 = d.y1;
    bt_det.score = d.score;
    bt_det.class_id = 0;  // person
    bt_dets.push_back(bt_det);
  }
  
  // Call ByteTrack
  auto bt_tracks = byte_tracker_->update(bt_dets, ts, fps);
  
  // Update our internal tracks map with ByteTrack results
  std::unordered_map<int, TrackStateInternal> new_tracks;
  
  for (const auto& bt_tr : bt_tracks) {
    // Get or create track
    auto it = tracks_.find(bt_tr.id);
    TrackStateInternal t;
    
    if (it != tracks_.end()) {
      // Existing track - update from KF box (NO bbox EMA!)
      t = it->second;
    } else {
      // New track - initialize
      t.id = bt_tr.id;
      t.state = bt_tr.confirmed ? ::TrackState::Confirmed : ::TrackState::Tentative;
      t.first_ts = ts;
      t.cx_prev = bt_tr.box.cx();
      t.cy_prev = bt_tr.box.cy();
      t.hist_idx = 0;
      t.filled = 0;
      t.vx = 0; t.vy = 0; t.speed = 0;
      t.dir_deg = 0; t.dir_label = "?"; t.dir_bin = -1;
      t.dir_conf = 0.f;
      t.moving_streak = 0;
      t.last_dir_ts = ts;
      t.dwell_s = 0.0;
      t.age_frames = 0;
      t.hits = 1;
      t.missed = 0;
      t.enter_sent = false;
      t.exit_sent = false;
      t.was_inside = false;
    }
    
    // Update bbox from KF (no EMA smoothing - KF already does this)
    t.x0 = bt_tr.box.x0;
    t.y0 = bt_tr.box.y0;
    t.x1 = bt_tr.box.x1;
    t.y1 = bt_tr.box.y1;
    t.score = bt_tr.score;
    t.age_frames = bt_tr.age;
    t.hits = bt_tr.hits;
    t.missed = bt_tr.time_since_update;
    
    // Use KF velocity directly with optional EMA for extra smoothness
    const float B = clamp01(cfg_.smooth_vel_alpha);  // e.g., 0.2
    t.vx = B * bt_tr.vx + (1.f - B) * t.vx;
    t.vy = B * bt_tr.vy + (1.f - B) * t.vy;
    
    // Debug: log KF velocity for first few frames (comment out in production)
    // if (t.age_frames < 100 && t.age_frames % 10 == 0) {
    //   float sp = std::hypot(t.vx, t.vy);
    //   fprintf(stderr, "[KF] id=%d age=%d c=(%.1f,%.1f) v=(%.1f,%.1f) sp=%.1f score=%.2f\n",
    //           t.id, t.age_frames, t.cx, t.cy, t.vx, t.vy, sp, t.score);
    // }
    
    // Update behavior fields (deadband, velocity, direction, ROI, dwell)
    update_behavior_fields(t, ts, fps);
    
    // Update state
    if (!bt_tr.confirmed && t.hits >= cfg_.confirm_hits) {
      t.state = ::TrackState::Confirmed;
    }
    
    new_tracks[t.id] = t;
  }
  
  tracks_.swap(new_tracks);
}
