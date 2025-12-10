#include "output/mqtt_publisher.h"
#include "metrics/log_global.h"
#include <chrono>
#include <cmath>   // For std::sqrt
#include <cstdio>
#include <fstream>
#include <string>

// Only compile full implementation if mosquitto is available
#if HAVE_MOSQUITTO

using clk = std::chrono::steady_clock;

// Helper: Read NPU load percentage from RK3568 kernel debug interface
static float read_npu_load_percent() {
  std::ifstream f("/sys/kernel/debug/rknpu/load");
  if (!f.is_open()) return 0.0f;
  
  std::string line;
  if (!std::getline(f, line)) return 0.0f;
  
  // Expected format: "NPU load: XX%"
  auto colon_pos = line.find(':');
  auto percent_pos = line.find('%');
  
  if (colon_pos != std::string::npos && 
      percent_pos != std::string::npos && 
      percent_pos > colon_pos) {
    try {
      return std::stof(line.substr(colon_pos + 1, percent_pos - colon_pos - 1));
    } catch (...) {
      return 0.0f;
    }
  }
  return 0.0f;
}

MqttPublisher::MqttPublisher(const Cfg& cfg) noexcept : cfg_(cfg) {
  mosquitto_lib_init();
}

MqttPublisher::~MqttPublisher() {
  stop();
  mosquitto_lib_cleanup();
}

bool MqttPublisher::start() noexcept {
  if (mq_) return true;
  mq_ = mosquitto_new(cfg_.client_id.c_str(), cfg_.clean_session, nullptr);
  if (!mq_) { 
    LG_ERROR("mqtt: mosquitto_new failed"); 
    return false; 
  }

  if (!cfg_.username.empty())
    mosquitto_username_pw_set(mq_, cfg_.username.c_str(), cfg_.password.c_str());

  if (mosquitto_connect(mq_, cfg_.host.c_str(), cfg_.port, /*keepalive*/30) != MOSQ_ERR_SUCCESS) {
    LG_ERROR("mqtt: connect to %s:%d failed", cfg_.host.c_str(), cfg_.port);
    mosquitto_destroy(mq_); 
    mq_ = nullptr; 
    return false;
  }

  if (mosquitto_loop_start(mq_) != MOSQ_ERR_SUCCESS) {
    LG_ERROR("mqtt: loop_start failed");
    mosquitto_disconnect(mq_); 
    mosquitto_destroy(mq_); 
    mq_ = nullptr; 
    return false;
  }

  run_.store(true, std::memory_order_release);
  tick_ = std::thread([this]{
    const int period = std::max(100, cfg_.period_ms);
    while (run_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(period));
      tick_publish();
    }
  });

  LG_INFO("mqtt: connected topic=%s qos=%d retain=%d period_ms=%d",
          cfg_.topic.c_str(), cfg_.qos, int(cfg_.retain), cfg_.period_ms);
  return true;
}

void MqttPublisher::stop() noexcept {
  run_.store(false, std::memory_order_release);
  if (tick_.joinable()) tick_.join();
  if (mq_) {
    mosquitto_loop_stop(mq_, /*force=*/false);
    mosquitto_disconnect(mq_);
    mosquitto_destroy(mq_);
    mq_ = nullptr;
  }
}

bool MqttPublisher::publish_result(const PipelineResult& r) noexcept {
  std::lock_guard<std::mutex> lk(m_);
  // Collect people_count (YOLOX) and gaze_count (RetinaFace)
  people_       = r.people_count;
  gaze_         = r.gaze_count;
  last_ts_ns_   = r.ts_ns;
  // Use FPS from PipelineResult instead of calculating from frames_accum_
  frames_accum_ = r.fps;  // Store actual FPS in frames_accum_ for use in payload
  frame_width_  = r.frame_width;   // V6.2: Store for normalized speed
  frame_height_ = r.frame_height;  // V6.2: Store for normalized speed
  
  // V7.0: Update health metrics from pipeline data (immediate, not waiting for telemetry)
  detector_fps_ = float(r.fps);  // Detector FPS ~= pipeline FPS
  tracker_fps_ = float(r.fps);   // Tracker FPS ~= pipeline FPS
  npu_load_ = read_npu_load_percent();  // Read NPU load from kernel debug interface
  
  // Store person tracks
  tracks_ = r.person_tracks;
  return true;
}

bool MqttPublisher::publish_telemetry(const TelemetrySnapshot& t) noexcept {
  std::lock_guard<std::mutex> lk(m_);
  // V7.0: Capture health metrics for MQTT output
  detector_fps_ = t.fps.avg_fps;     // Use average FPS as detector rate
  tracker_fps_ = t.fps.avg_fps;      // Same for tracker (can differentiate if needed)
  npu_load_ = t.npu_util * 100.0f;   // Convert 0-1 to 0-100%
  dropped_frames_ = 0;                // TODO: wire up from error counters if available
  // last_model_reload_ts_ remains 0.0 unless model reloading is implemented
  return true;
}

std::string MqttPublisher::make_payload_locked() const {
  // Build v7.0 JSON with full schema
  const int fps = frames_accum_;
  const double ts_s = last_ts_ns_ * 1e-9;  // Convert ns to seconds
  
  // CRITICAL FIX: Derive people count from actual tracks we're about to emit
  const int people_count = static_cast<int>(tracks_.size());
  
  // Count high-confidence tracks (score >= 0.70)
  int people_confident = 0;
  for (const auto& t : tracks_) {
    if (t.score >= 0.70f) people_confident++;
  }
  
  std::string payload;
  payload.reserve(1024 + tracks_.size() * 256);  // Larger buffer for v7.0
  
  // V7.0: Full schema with metadata
  char header[768];
  std::snprintf(header, sizeof(header),
    "{\"schema\":\"analytics/v7.0\","
    "\"ts\":%.2f,\"device\":\"%s\",\"stream\":\"%s\","
    "\"frame_w\":%d,\"frame_h\":%d,"
    "\"model\":\"yolox_s\",\"fw_version\":\"7.0.0\","
    "\"npu_load\":%.1f,"
    "\"people\":%d,\"people_confident\":%d,"
    "\"gaze\":%d,\"fps\":%d,"
    "\"roi\":{\"type\":\"border\",\"border_frac\":0.30,\"rect\":[%d,%d,%d,%d]},"
    "\"health\":{\"detector_fps\":%.1f,\"tracker_fps\":%.1f,\"queue_latency_ms\":0,\"dropped_frames\":%d,\"last_model_reload_ts\":%.1f},"
    "\"tracks\":[",
    ts_s,
    cfg_.device_id.c_str(),
    cfg_.stream_id.c_str(),
    frame_width_, frame_height_,
    npu_load_,
    people_count, people_confident,
    gaze_, fps,
    // ROI rect (30% border inset - increased to handle fast-moving people and detection gaps)
    int(frame_width_ * 0.30f), int(frame_height_ * 0.30f),
    int(frame_width_ * 0.70f), int(frame_height_ * 0.70f),
    detector_fps_, tracker_fps_, dropped_frames_, last_model_reload_ts_);
  payload += header;
  
  // Add each track with v7.0 fields
  for (size_t i = 0; i < tracks_.size(); ++i) {
    const auto& t = tracks_[i];
    
    // V7.1b: Calculate frame diagonal once (used for normalized speed and speed floor)
    const float frame_diag = std::sqrt(float(frame_width_ * frame_width_ + frame_height_ * frame_height_));
    
    // V6.2: Calculate normalized speed (% of frame diagonal per second)
    float speed_norm = (frame_diag > 0) ? (t.speed / frame_diag) : 0.0f;
    
    // V7.0: Determine zones (simplified - just "roi" vs "edge")
    // V7.2b: Use same border_frac (30%) as tracker for consistency
    const float cx = (t.x0 + t.x1) * 0.5f;
    const float cy = (t.y0 + t.y1) * 0.5f;
    const float border_frac = 0.30f;  // Match tracker's enter_exit_border_frac
    const float border_px_x = frame_width_ * border_frac;   // 192px for 640px frame
    const float border_px_y = frame_height_ * border_frac;  // 144px for 480px frame
    const bool is_edge = (cx < border_px_x || cx > frame_width_ - border_px_x ||
                          cy < border_px_y || cy > frame_height_ - border_px_y);
    const char* zones_str = is_edge ? "[\"edge\"]" : "[\"roi\"]";
    
    // V7.1b: Strict publisher gate with resolution-adaptive threshold
    // Prevents held directions from appearing when speed dips below threshold
    // Uses diagonal-based relative floor (0.6% of frame diagonal per second)
    // 
    // V7.1d: Trust tracker's hold logic - only check confidence and label
    // Tracker handles speed floor, hold windows, and confidence decay internally
    constexpr float MIN_DIR_CONF = 0.35f;          // Match tracker's reset threshold
    const bool has_direction = (t.dir_label[0] != '?' || t.dir_label[1] != '\0');  // Not "?"
    const bool show_direction = (t.dir_conf >= MIN_DIR_CONF) && has_direction;
    
    // Publish direction from tracker (already handles hold logic)
    const char* pub_dir = show_direction ? t.dir_label : "?";
    const float pub_deg = show_direction ? t.dir_deg : 0.0f;
    const float pub_conf = show_direction ? t.dir_conf : 0.0f;
    
    // V7.1b: Cosmetic - zero tiny speeds for cleaner telemetry
    const float pub_speed = (t.speed >= 2.0f) ? t.speed : 0.0f;
    
    char buf[768];  // Larger buffer for v7.0 + gaze data
    
    // Build gaze object if available
    if (t.has_gaze) {
      // DEBUG: Log every gaze publish for better timing visibility
      LG_INFO("[MQTT-GAZE] Track %d: gaze=%d, time=%.2f, face=[%.0f,%.0f,%.0f,%.0f]",
              t.id, t.is_gazing ? 1 : 0, t.gaze_time, 
              t.face_bbox_x0, t.face_bbox_y0, t.face_bbox_x1, t.face_bbox_y1);
      
      std::snprintf(buf, sizeof(buf),
        "{\"id\":%d,\"state\":\"Confirmed\","
        "\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"score\":%.2f,"
        "\"zones\":%s,"
        "\"dir\":\"%s\",\"deg\":%.1f,\"dir_conf\":%.2f,"
        "\"speed\":%.1f,\"speed_norm\":%.3f,"
        "\"dwell\":%.2f,\"enter\":%s,\"exit\":%s,"
        "\"gaze\":{\"detected\":%d,\"time\":%.2f,\"face_bbox\":[%.1f,%.1f,%.1f,%.1f]}}",
        t.id,
        t.x0, t.y0, t.x1, t.y1, t.score,
        zones_str,
        pub_dir, pub_deg, pub_conf,  // V7.1b: Use gated values instead of raw tracker values
        pub_speed, speed_norm,        // V7.1b: Use cleaned speed (zero if < 2.0 px/s)
        t.dwell_s,
        t.just_entered ? "true" : "false",
        t.just_exited ? "true" : "false",
        t.is_gazing ? 1 : 0,
        t.gaze_time,
        t.face_bbox_x0, t.face_bbox_y0, t.face_bbox_x1, t.face_bbox_y1);
    } else {
      // No gaze data available for this track
      // DEBUG: Reduced logging for no-gaze tracks
      static int mqtt_no_gaze_log_counter = 0;
      if (++mqtt_no_gaze_log_counter % 20 == 0) {  // Log every 20th to reduce noise
        LG_DEBUG("[MQTT-NO-GAZE] Track %d has no gaze data (has_gaze=false)", t.id);
      }
      
      std::snprintf(buf, sizeof(buf),
        "{\"id\":%d,\"state\":\"Confirmed\","
        "\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"score\":%.2f,"
        "\"zones\":%s,"
        "\"dir\":\"%s\",\"deg\":%.1f,\"dir_conf\":%.2f,"
        "\"speed\":%.1f,\"speed_norm\":%.3f,"
        "\"dwell\":%.2f,\"enter\":%s,\"exit\":%s}",
        t.id,
        t.x0, t.y0, t.x1, t.y1, t.score,
        zones_str,
        pub_dir, pub_deg, pub_conf,  // V7.1b: Use gated values instead of raw tracker values
        pub_speed, speed_norm,        // V7.1b: Use cleaned speed (zero if < 2.0 px/s)
        t.dwell_s,
        t.just_entered ? "true" : "false",
        t.just_exited ? "true" : "false");
    }
    
    if (i > 0) payload += ",";
    payload += buf;
  }
  
  payload += "]}";
  return payload;
}

void MqttPublisher::tick_publish() noexcept {
  if (!mq_) return;
  std::string payload;
  {
    std::lock_guard<std::mutex> lk(m_);
    payload = make_payload_locked();
    // Don't reset frames_accum_ here anymore - it holds the actual FPS
  }
  const int rc = mosquitto_publish(mq_, nullptr, cfg_.topic.c_str(),
                                   int(payload.size()), payload.data(),
                                   cfg_.qos, cfg_.retain);
  if (rc != MOSQ_ERR_SUCCESS) {
    LG_WARN("mqtt: publish failed rc=%d", rc);
  }
}

#endif // HAVE_MOSQUITTO
