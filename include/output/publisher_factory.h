#ifndef PUBLISHER_FACTORY_H
#define PUBLISHER_FACTORY_H

#include <memory>
#include <vector>
#include "publisher.h"
#include "udp_json_publisher.h"
#include "brightsign_v3_publisher.h"
#include "publisher_config.h"  // from your Configuration module

// Build one publisher from config
inline PublisherPtr make_publisher(const PublisherConfig& cfg) {
  switch (cfg.kind) {
    case PublisherKind::UDP:
      return std::make_unique<UdpJsonPublisher>(UdpEndpoint{cfg.udp.host, cfg.udp.port});
    case PublisherKind::Stdout:
      // simple stdout publisher could be implemented in .cpp if needed
      return nullptr;
    case PublisherKind::Mqtt:
      // implement later
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

