#include "config/model_spec.h"
#include <cstdio>

bool ModelSpec::validate(char* err, size_t err_sz) const noexcept {
  if (model_path.empty()) { if (err&&err_sz) std::snprintf(err, err_sz, "model_path empty"); return false; }
  if (input_size.w<=0 || input_size.h<=0) { if (err&&err_sz) std::snprintf(err, err_sz, "invalid input size"); return false; }
  if (!(norm.channels==1 || norm.channels==3 || norm.channels==4)) { if (err&&err_sz) std::snprintf(err, err_sz, "channels must be 1/3/4"); return false; }
  if (conf_threshold<0.f || conf_threshold>1.f) { if (err&&err_sz) std::snprintf(err, err_sz, "conf_threshold out of range"); return false; }
  if (nms_threshold<0.f || nms_threshold>1.f) { if (err&&err_sz) std::snprintf(err, err_sz, "nms_threshold out of range"); return false; }
  return true;
}

