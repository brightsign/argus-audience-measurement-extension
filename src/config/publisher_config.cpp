#include "config/publisher_config.h"
#include <cstdio>

bool PublisherConfig::validate(char* err, size_t err_sz) const noexcept {
  if (kind==PublisherKind::UDP && (udp.port==0 || udp.host.empty())) {
    if (err&&err_sz) std::snprintf(err, err_sz, "invalid UDP endpoint"); return false;
  }
  return true;
}

