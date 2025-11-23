#pragma once
#include "byte_types.h"
#include "kf_diag2d.h"
#include <unordered_map>
#include <vector>

namespace bytetrack {

struct ByteTrackLiteCfg {
  float det_high = 0.50f;   // high threshold
  float det_low  = 0.15f;   // low threshold
  float match_iou_high = 0.70f; // IoU for stage-1 (high)
  float match_iou_low  = 0.50f; // IoU for stage-2 (low)
  int   max_age   = 30;     // frames to keep unmatched tracks
  int   n_init    = 3;      // hits to confirm
};

class ByteTrackLite {
public:
  explicit ByteTrackLite(const ByteTrackLiteCfg& c) : cfg_(c) {}

  // Input: raw detections (camera-space) for 'person' class
  // Output: current active tracks (confirmed + tentative)
  std::vector<TrackDesc> update(const std::vector<Detection>& dets, double ts, double fps);

  // Internals exposed just for integration if needed:
  struct Trk {
    int id=0;
    KFDiag2D kf;
    float score=0.f;
    int age=0;
    int hits=0;
    int time_since_update=0;
    bool confirmed=false;
  };

  const std::vector<Trk>& tracks() const { return tracks_; }

private:
  ByteTrackLiteCfg cfg_;
  std::vector<Trk> tracks_;
  int next_id_ = 1;
  double last_ts_ = -1.0;

  static float bbox_distance_1miou(const BBox& a, const BBox& b) { return 1.0f - iou(a,b); }

  static void greedy_assign_iou(const std::vector<BBox>& A,
                                const std::vector<BBox>& B,
                                float iou_thresh,
                                std::vector<std::pair<int,int>>& matches,
                                std::vector<int>& unmA,
                                std::vector<int>& unmB);

  void predict_all(double dt);
  void start_track(const Detection& d);
  void update_track(Trk& t, const Detection& d);
  void mark_missed(Trk& t);
  static BBox kf_bbox(const Trk& t) { return t.kf.to_bbox(); }
};

}  // namespace bytetrack
