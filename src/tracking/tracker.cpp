#include "tracking/tracker.h"
#include "models/model_runner.h"  // For Detection struct definition
#include <algorithm>
#include <cmath>

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

  // --- robust dt from timestamps ---
  const double frame_T = (fps > 0 ? 1.0 / fps : 0.0333333); // ~30 fps fallback
  double dt = ts - t.last_ts;
  // clamp dt to reasonable range (handles duplicate/late timestamps, timer jitter)
  // Widened to [0.4*frame_T .. 2.5*frame_T] for more tolerance
  if (!std::isfinite(dt) || dt <= frame_T * 0.4 || dt >= frame_T * 2.5)
    dt = frame_T;

  // Update history buffer for deadband check
  t.cx_hist[t.hist_idx] = t.cx;
  t.cy_hist[t.hist_idx] = t.cy;
  t.hist_idx = (t.hist_idx + 1) % t.HIST_SIZE;
  
  // Adaptive deadband: 1% of diagonal, clamped [2px..cfg.motion_deadband_px]
  // Smaller boxes (closer subjects) need less displacement to be "moving"
  float diag = std::hypot(t.w, t.h);
  float adaptive_db = std::clamp(0.01f * diag, 2.0f, cfg_.motion_deadband_px);
  
  // Deadband check: calculate displacement over history window
  int oldest_idx = t.hist_idx;  // Circular buffer, oldest is at current write position
  float dx_hist = t.cx - t.cx_hist[oldest_idx];
  float dy_hist = t.cy - t.cy_hist[oldest_idx];
  float displacement = std::hypot(dx_hist, dy_hist);
  
  // ROI enter/exit and dwell tracking
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
  
  // If displacement is below adaptive deadband, suppress motion (jitter)
  if (displacement < adaptive_db) {
    // Jitter detected - keep previous velocity/direction, output as stationary
    t.dir_label = "?";
    t.speed = 0.f;
    t.dir_deg = 0.f;  // Blank angle when direction unknown
    // Don't update dir_bin when stationary
  } else {
    // Real motion detected - compute per-step (smoothed) velocity from current step
    // Apply tiny per-step deadband before dividing by dt
    float dcx = t.cx - t.cx_prev;
    float dcy = t.cy - t.cy_prev;

    // Tighter sub-pixel gate to kill micro-wobble (raised from 0.25 to 0.40)
    const float step_db = 0.40f;
    if (std::fabs(dcx) < step_db) dcx = 0.f;
    if (std::fabs(dcy) < step_db) dcy = 0.f;

    float inst_vx = (dt > 0) ? (dcx / (float)dt) : 0.f;   // px/s
    float inst_vy = (dt > 0) ? (dcy / (float)dt) : 0.f;   // px/s

    // Smooth velocity with EMA (reduces spikes)
    float B = clamp01(cfg_.smooth_vel_alpha);
    t.vx = B * inst_vx + (1.f - B) * t.vx;
    t.vy = B * inst_vy + (1.f - B) * t.vy;

    float sp = std::hypot(t.vx, t.vy); // px/s
    
    // Debug: verify coordinate space and velocity for first few updates
    static int debug_count = 0;
    if (debug_count < 30) {  // Log first 30 updates (~1 second @ 30fps)
      fprintf(stderr,
        "[TRK_DBG] id=%d cx=%.1f cy=%.1f prev=%.1f,%.1f dt=%.3f dc=(%.2f,%.2f) v=(%.1f,%.1f) sp=%.1f\n",
        t.id, t.cx, t.cy, t.cx_prev, t.cy_prev, dt,
        t.cx - t.cx_prev, t.cy - t.cy_prev, t.vx, t.vy, sp);
      debug_count++;
    }
    
    // Motion floor in px/s (per-second, not frame-based)
    const float stationary_eps = std::max(0.0f, cfg_.min_speed_px_s); // e.g., 24 px/s typical
    
    // Calculate raw direction angle
    float deg = std::atan2(-t.vy, t.vx) * 180.f / float(M_PI);
    if (deg < 0.f) deg += 360.f;
    
    // Dampen direction when detection score is low (V6.2)
    const float low_score_thresh = cfg_.low_score_thresh;
    float dir_weight = (t.score < low_score_thresh) ? 0.3f : 1.0f;
    
    // Blend angle toward previous direction when score is low
    if (dir_weight < 1.0f && t.dir_conf > 0.0f && t.dir_bin >= 0) {
      float prev_deg = t.dir_deg;
      // Shortest angular difference
      float d = deg - prev_deg;
      while (d > 180.f) d -= 360.f;
      while (d < -180.f) d += 360.f;
      deg = prev_deg + dir_weight * d;
      if (deg < 0.f) deg += 360.f;
      if (deg >= 360.f) deg -= 360.f;
    }
    
    // Sticky direction with decay (V6.2) - keeps last good direction during brief slow moments
    if (sp >= stationary_eps) {
      // Moving above threshold: accept new direction, reset confidence
      t.dir_deg = deg;
      
      // Calculate current bin (0-7 for 8-way compass)
      int bin_now = static_cast<int>(std::round(deg / 45.f)) & 7;
      
      // Direction bin hysteresis: only change bin if we cross midline significantly
      if (t.dir_bin == -1) {
        // First time - initialize
        t.dir_bin = bin_now;
      } else {
        // Enhanced bin hysteresis: check distance to nearest bin midline
        float current_bin_center = t.dir_bin * 45.f;
        
        // Calculate angular distance to current bin center
        float angular_diff = deg - current_bin_center;
        
        // Normalize to [-180, 180]
        while (angular_diff > 180.f) angular_diff -= 360.f;
        while (angular_diff < -180.f) angular_diff += 360.f;
        
        // Only switch bin if we've crossed midline by margin (reduced flapping)
        if (std::fabs(angular_diff) > (22.5f + cfg_.bin_hysteresis_deg)) {
          t.dir_bin = bin_now;
        }
      }
      
      t.dir_label = dir_label_from_deg(t.dir_bin * 45.f);
      
      // V6.2.3: Temporarily raise clamp to diagnose spikes
      const float max_speed_px_s = 500.0f;  // Raised from 180 to see raw values
      t.speed = std::min(sp, max_speed_px_s);
      
      // V6.2.2: Compute actual direction confidence (0..1)
      // Based on: 1) speed relative to threshold, 2) angular stability within bin
      const float min_speed_px_s = std::max(0.0f, cfg_.min_speed_px_s);
      float sp_rel = std::min(1.0f, sp / (min_speed_px_s * 3.0f)); // 0..1 (full conf at 3x threshold)
      
      // Penalize jitter near bin boundaries (closer to bin center → higher confidence)
      float bin_center = t.dir_bin * 45.f;
      float ang_err = deg - bin_center;
      // Normalize to [-180, 180]
      while (ang_err > 180.f) ang_err -= 360.f;
      while (ang_err < -180.f) ang_err += 360.f;
      ang_err = std::fabs(ang_err);  // 0..180
      float center_score = std::max(0.f, 1.f - (ang_err / 45.f)); // 1 at center, 0 at edges
      
      t.dir_conf = std::max(0.0f, std::min(1.0f, 0.5f*sp_rel + 0.5f*center_score));
      t.last_dir_ts = ts;
    } else {
      // Slow or jitter: keep last direction for a while, decay confidence
      double dt_dir = std::max(0.0, ts - t.last_dir_ts);
      if (dt_dir * 1000.0 <= cfg_.dir_hold_ms && t.dir_conf > 0.15f) {
        // Keep previous direction, decay confidence
        t.dir_conf = std::max(0.0f, t.dir_conf - float(cfg_.dir_decay_per_s * dt_dir));
        // Leave t.dir_label, t.dir_deg, t.dir_bin as-is (sticky)
        t.speed = sp;  // But show actual (low) speed
      } else {
        // Too long slow / no confidence -> unknown
        t.dir_label = "?";
        t.dir_deg = 0.0f;
        t.speed = 0.f;
        t.dir_conf = 0.0f;
      }
    }
  }
  
  // Update previous center for next iteration
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
