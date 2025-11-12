#ifndef PUBLISHER_CONFIG_H
#define PUBLISHER_CONFIG_H

#include "config/config_common.h"
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

struct MqttPublisherConfig {
  std::string host{"127.0.0.1"};
  int         port{1883};
  std::string client_id{"xt5-gaze"};
  std::string topic{"bs/argus/analytics"};
  int         qos{1};
  bool        retain{false};
  int         period_ms{1000};
  std::string username{};
  std::string password{};
  bool        clean_session{true};
};

struct PublisherConfig {
  PublisherKind kind{PublisherKind::Stdout};
  UdpPublisher  udp{};
  FilePublisher file{};
  MqttPublisherConfig mqtt{};
  // Simple switch: choose fields by 'kind'
  bool validate(char* err, size_t err_sz) const noexcept;
};

#endif // PUBLISHER_CONFIG_H

