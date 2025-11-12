#include "output/mqtt_publisher.h"
#include "metrics/log_global.h"
#include <chrono>
#include <cstdio>

// Only compile full implementation if mosquitto is available
#if HAVE_MOSQUITTO

using clk = std::chrono::steady_clock;

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
  return true;
}

bool MqttPublisher::publish_telemetry(const TelemetrySnapshot& t) noexcept {
  // Optional: could add telemetry (temperature, etc.)
  (void)t;
  return true;
}

std::string MqttPublisher::make_payload_locked() const {
  // keep tiny and dependency-free (no nlohmann JSON)
  // Use frames_accum_ which now stores the actual FPS from PipelineResult
  const int fps = frames_accum_;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
    "{\"ts\":%llu,\"people\":%d,\"gaze\":%d,\"fps\":%d}",
    (unsigned long long)(last_ts_ns_ / 1000000ULL),
    people_, gaze_, fps);
  return std::string(buf);
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
