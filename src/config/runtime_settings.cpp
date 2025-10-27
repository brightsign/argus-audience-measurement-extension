#include "config/runtime_settings.h"
#include <cstdio>

bool RuntimeSettings::validate(char* err, size_t err_sz) const noexcept {
  if (target_fps<0) { if (err&&err_sz) std::snprintf(err, err_sz, "target_fps < 0"); return false; }
  if (heartbeat_ms<=0) { if (err&&err_sz) std::snprintf(err, err_sz, "heartbeat_ms <= 0"); return false; }
  return true;
}

