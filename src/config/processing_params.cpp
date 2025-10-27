#include "config/processing_params.h"
#include <cstdio>

bool ProcessingParams::validate(char* err, size_t err_sz) const noexcept {
  if (th.score<0.f || th.score>1.f) { if (err&&err_sz) std::snprintf(err, err_sz, "score out of range"); return false; }
  if (th.iou<0.f || th.iou>1.f)     { if (err&&err_sz) std::snprintf(err, err_sz, "iou out of range");   return false; }
  return true;
}

