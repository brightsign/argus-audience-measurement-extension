#include "tracking/byte_tracker_lite.h"
#include <limits>

namespace bytetrack {

void ByteTrackLite::predict_all(double dt) {
  for (auto& t : tracks_) {
    t.kf.predict((float)dt);
    t.age++;
    t.time_since_update++;
  }
}

void ByteTrackLite::start_track(const Detection& d) {
  Trk t;
  t.id = next_id_++;
  t.kf.init(d.box);
  t.score = d.score;
  t.age = 1;
  t.hits = 1;
  t.time_since_update = 0;
  t.confirmed = (t.hits >= cfg_.n_init);
  tracks_.push_back(t);
}

void ByteTrackLite::update_track(Trk& t, const Detection& d) {
  t.kf.update(d.box);
  t.score = d.score;
  t.time_since_update = 0;
  t.hits++;
  if (!t.confirmed && t.hits >= cfg_.n_init) t.confirmed = true;
}

void ByteTrackLite::mark_missed(Trk& t) {
  // nothing special here; pruning happens after association
}

void ByteTrackLite::greedy_assign_iou(const std::vector<BBox>& A,
                                      const std::vector<BBox>& B,
                                      float iou_thresh,
                                      std::vector<std::pair<int,int>>& matches,
                                      std::vector<int>& unmA,
                                      std::vector<int>& unmB)
{
  const int NA = (int)A.size(), NB = (int)B.size();
  std::vector<int> usedB(NB, 0);
  for (int i=0; i<NA; ++i) {
    float best = -1.f; int bestj = -1;
    for (int j=0; j<NB; ++j) {
      if (usedB[j]) continue;
      const float IoU = iou(A[i], B[j]);
      if (IoU > best) { best = IoU; bestj = j; }
    }
    if (bestj >= 0 && best >= iou_thresh) {
      matches.emplace_back(i, bestj);
      usedB[bestj] = 1;
    } else {
      unmA.push_back(i);
    }
  }
  for (int j=0;j<NB;++j) if (!usedB[j]) unmB.push_back(j);
}

std::vector<TrackDesc> ByteTrackLite::update(const std::vector<Detection>& dets_in, double ts, double fps)
{
  // Split detections by score
  std::vector<Detection> dets_high, dets_low;
  dets_high.reserve(dets_in.size());
  dets_low.reserve(dets_in.size());
  for (auto& d : dets_in) {
    if (d.class_id != 0) continue; // only persons
    if (d.score >= cfg_.det_high) dets_high.push_back(d);
    else if (d.score >= cfg_.det_low) dets_low.push_back(d);
  }

  // Time step
  double dt = (last_ts_ < 0.0) ? (1.0 / std::max(1.0, fps)) : std::max(1e-3, ts - last_ts_);
  last_ts_ = ts;

  // Predict all
  predict_all(dt);

  // Collect track-bboxes for association
  std::vector<int> live_idx; live_idx.reserve(tracks_.size());
  std::vector<BBox> live_box; live_box.reserve(tracks_.size());
  for (int i=0;i<(int)tracks_.size();++i) {
    live_idx.push_back(i);
    live_box.push_back(kf_bbox(tracks_[i]));
  }

  // Stage 1: match HIGH score detections
  std::vector<std::pair<int,int>> m1;
  std::vector<int> u_trk1, u_det1;
  {
    std::vector<BBox> det_boxes; det_boxes.reserve(dets_high.size());
    for (auto& d : dets_high) det_boxes.push_back(d.box);
    greedy_assign_iou(live_box, det_boxes, cfg_.match_iou_high, m1, u_trk1, u_det1);
  }

  // Apply stage-1 updates
  std::vector<int> unmatched_tracks = u_trk1;
  std::vector<int> unmatched_det_idx = u_det1; // indices into dets_high
  for (auto& p : m1) {
    int ti = live_idx[p.first];
    int di = p.second;
    update_track(tracks_[ti], dets_high[di]);
  }

  // Stage 2: use LOW score detections to recover remaining tracks
  if (!unmatched_tracks.empty() && !dets_low.empty()) {
    std::vector<BBox> ut_boxes; ut_boxes.reserve(unmatched_tracks.size());
    for (int k=0;k<(int)unmatched_tracks.size();++k)
      ut_boxes.push_back(kf_bbox(tracks_[ unmatched_tracks[k] ]));

    std::vector<BBox> low_boxes; low_boxes.reserve(dets_low.size());
    for (auto& d : dets_low) low_boxes.push_back(d.box);

    std::vector<std::pair<int,int>> m2;
    std::vector<int> u_trk2, u_det2;
    greedy_assign_iou(ut_boxes, low_boxes, cfg_.match_iou_low, m2, u_trk2, u_det2);

    // apply matches
    for (auto& p : m2) {
      int ti_global = unmatched_tracks[p.first];
      int di = p.second;
      update_track(tracks_[ti_global], dets_low[di]);
    }

    // update unmatched list after stage-2
    std::vector<int> still_unmatched;
    // those ut indices in u_trk2 remain unmatched
    for (int idx : u_trk2) {
      still_unmatched.push_back(unmatched_tracks[idx]);
    }
    unmatched_tracks.swap(still_unmatched);
  }

  // Start new tracks from unmatched HIGH score dets only (ByteTrack rule)
  if (!unmatched_det_idx.empty()) {
    for (int di : unmatched_det_idx) {
      start_track(dets_high[di]);
    }
  }

  // Age-out unmatched tracks
  std::vector<Trk> kept;
  kept.reserve(tracks_.size());
  for (auto& t : tracks_) {
    if (t.time_since_update > 0) mark_missed(t);
    // prune if too old OR box degenerate
    if (t.time_since_update <= cfg_.max_age && t.kf.to_bbox().w() > 1 && t.kf.to_bbox().h() > 1) {
      kept.push_back(t);
    }
  }
  tracks_.swap(kept);

  // Build public output
  std::vector<TrackDesc> out; out.reserve(tracks_.size());
  for (auto& t : tracks_) {
    out.push_back(TrackDesc{
      t.id, t.kf.to_bbox(), t.score, t.confirmed, t.time_since_update, t.age, t.hits,
      t.kf.vx, t.kf.vy   // NEW: KF velocity
    });
  }
  return out;
}

}  // namespace bytetrack
