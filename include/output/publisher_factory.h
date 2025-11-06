#ifndef PUBLISHER_FACTORY_H
#define PUBLISHER_FACTORY_H

#include <memory>
#include <vector>
#include "output/publisher_v2.h"
#include "output/async_publisher.h"
#include "output/udp_json_publisher.h"
#include "output/brightsign_v3_publisher.h"
#include "output/mqtt_publisher.h"
#include "config/publisher_config.h"  // from your Configuration module

// Build one publisher from config
inline PublisherPtr make_publisher(const PublisherConfig& cfg) {
  switch (cfg.kind) {
    case PublisherKind::UDP:
      return std::make_unique<UdpJsonPublisher>(UdpEndpoint{cfg.udp.host, cfg.udp.port});
    
    case PublisherKind::Mqtt: {
      MqttPublisher::Cfg mc{};
      mc.host         = cfg.mqtt.host;
      mc.port         = cfg.mqtt.port;
      mc.client_id    = cfg.mqtt.client_id;
      mc.topic        = cfg.mqtt.topic;
      mc.qos          = cfg.mqtt.qos;
      mc.retain       = cfg.mqtt.retain;
      mc.period_ms    = cfg.mqtt.period_ms;
      mc.username     = cfg.mqtt.username;
      mc.password     = cfg.mqtt.password;
      mc.clean_session= cfg.mqtt.clean_session;
      return std::make_unique<MqttPublisher>(mc);
    }
    
    case PublisherKind::Stdout:
      // simple stdout publisher could be implemented in .cpp if needed
      return nullptr;
    
    default:
      return nullptr;
  }
}

// Build and wrap publishers with AsyncPublisher
inline std::vector<PublisherPtr> make_async_publishers(const std::vector<PublisherConfig>& cfgs,
                                                       size_t queue_capacity = 64) {
  std::vector<PublisherPtr> out;
  out.reserve(cfgs.size());
  for (const auto& c : cfgs) {
    if (auto p = make_publisher(c)) {
      auto ap = std::make_unique<AsyncPublisher>(std::move(p), AsyncPublisher::Config{queue_capacity});
      out.emplace_back(std::move(ap));
    }
  }
  return out;
}

#endif // PUBLISHER_FACTORY_H

