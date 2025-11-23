#pragma once
#include <vector>
#include <cmath>
#include <cfloat>
#include <algorithm>

namespace bytetrack {

struct BBox {
  float x0, y0, x1, y1; // camera-space, inclusive-exclusive doesn't matter as long as consistent
  float w() const { return std::max(0.f, x1 - x0); }
  float h() const { return std::max(0.f, y1 - y0); }
  float cx() const { return 0.5f * (x0 + x1); }
  float cy() const { return 0.5f * (y0 + y1); }
};

inline float iou(const BBox& a, const BBox& b) {
  const float xx0 = std::max(a.x0, b.x0);
  const float yy0 = std::max(a.y0, b.y0);
  const float xx1 = std::min(a.x1, b.x1);
  const float yy1 = std::min(a.y1, b.y1);
  const float iw = std::max(0.f, xx1 - xx0);
  const float ih = std::max(0.f, yy1 - yy0);
  const float inter = iw * ih;
  const float ua = a.w() * a.h() + b.w() * b.h() - inter + 1e-6f;
  return inter / ua;
}

struct Detection {
  BBox box;
  float score;
  int   class_id; // 0 for person in your case
};

struct TrackDesc {
  int   id;
  BBox  box;      // KF-smoothed box (camera space)
  float score;    // last detection score
  bool  confirmed;
  int   time_since_update; // frames since matched
  int   age;               // frames total
  int   hits;              // matched frames
  // NEW: KF velocity (px/s)
  float vx;
  float vy;
};

}  // namespace bytetrack
