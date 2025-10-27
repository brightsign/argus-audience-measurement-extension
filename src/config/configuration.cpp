#include "config/configuration.h"
#include <cstdio>

bool AppConfig::validate(char* err, size_t err_sz) const noexcept {
  char tmp[128]{};
  if (!primary_model.validate(tmp, sizeof(tmp))) {
    if (err&&err_sz) std::snprintf(err, err_sz, "model invalid: %s", tmp);
    return false;
  }
  if (!processing.validate(tmp, sizeof(tmp))) {
    if (err&&err_sz) std::snprintf(err, err_sz, "processing invalid: %s", tmp);
    return false;
  }
  if (!runtime.validate(tmp, sizeof(tmp))) {
    if (err&&err_sz) std::snprintf(err, err_sz, "runtime invalid: %s", tmp);
    return false;
  }
  for (const auto& p : publishers) {
    if (!p.validate(tmp, sizeof(tmp))) {
      if (err&&err_sz) std::snprintf(err, err_sz, "publisher invalid: %s", tmp);
      return false;
    }
  }
  return true;
}

namespace config {
bool load_from_file(const std::string& path, AppConfig& out, bool /*strict*/, char* err, size_t err_sz) noexcept {
  // Minimal placeholder: produce defaults if file path exists string-wise.
  if (path.empty()) { if (err&&err_sz) std::snprintf(err, err_sz, "empty path"); return false; }
  // TODO: parse JSON/TOML here.
  out.primary_model.model_path = "/app/models/retinaface.rknn";
  out.primary_model.name = "retinaface";
  out.primary_model.input_size = {320,320};
  out.primary_model.norm.channels = 3;
  out.processing.th.score = 0.5f;
  out.runtime.heartbeat_ms = 1000;
  out.input.rtsp_url.clear(); // user to fill
  return true;
}
bool save_to_file(const std::string& /*path*/, const AppConfig& /*in*/, char* /*err*/, size_t /*err_sz*/) noexcept {
  return false;
}
}

