#ifndef PUBLISHER_CONFIG_H
#define PUBLISHER_CONFIG_H

#include "config_common.h"
#include <string>

struct UdpPublisher {
  std::string host{"127.0.0.1"};
  uint16_t    port{5555};
};

struct FilePublisher {
  std::string dir{"/storage/sd/out"};
  bool        rotate{true};
  size_t      max_bytes{2 * 1024 * 1024}; // per file
};

struct MqttPublisher {
  std::string broker{"tcp://localhost:1883"};
  std::string topic{"inference/detections"};
  std::string client_id{"xt5-ml"};
  bool        retain{false};
  bool        qos1{false};
};

struct PublisherConfig {
  PublisherKind kind{PublisherKind::Stdout};
  UdpPublisher  udp{};
  FilePublisher file{};
  MqttPublisher mqtt{};
  // Simple switch: choose fields by 'kind'
  bool validate(char* err, size_t err_sz) const noexcept;
};

#endif // PUBLISHER_CONFIG_H

