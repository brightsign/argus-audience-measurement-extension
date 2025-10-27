#ifndef PROCESSING_PARAMS_H
#define PROCESSING_PARAMS_H

#include "config/config_common.h"

struct Thresholds {
  float score{0.5f};          // detection/confidence threshold
  float iou{0.45f};           // for NMS
  float keypoint{0.2f};       // e.g., facial landmarks
};

struct NmsSettings {
  NmsMethod method{NmsMethod::Greedy};
  float     sigma{0.5f};      // for Soft-NMS
  int       top_k{100};       // limit for proposals
  bool      class_agnostic{true};
};

struct ProcessingParams {
  Thresholds  th{};
  NmsSettings nms{};

  // Task-specific knobs (optional)
  bool  keep_aspect{true};    // letterbox vs stretch
  float letterbox_value{0.f}; // 0 black, 128/255 gray, etc.

  bool validate(char* err, size_t err_sz) const noexcept;
};

#endif // PROCESSING_PARAMS_H

