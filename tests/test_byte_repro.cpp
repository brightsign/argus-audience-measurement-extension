// Regression guard: the ByteTrack core (as configured in the orchestrator) must
// produce a confirmed track from a steady person-sized detection. This failed when
// update_with_bytetrack() never promoted a byte-confirmed track to Confirmed, so
// the tracker emitted nothing and "people" stayed 0 on the dashboard.
#include <gtest/gtest.h>
#include "tracking/tracker.h"
#include "models/model_runner.h"
#include <vector>

static TrackerConfig orch_byte_cfg() {
  TrackerConfig c;
  c.tracker_core = "byte";
  c.byte_max_age = 30;
  c.iou_match_thresh = 0.35f;
  c.confirm_hits = 2;
  c.max_missed = 8;
  c.min_det_score = 0.35f;
  c.min_area_px = 1600;
  return c;
}

static Detection person(float x0,float y0,float x1,float y1,float s=0.9f) {
  Detection d; d.x0=x0; d.y0=y0; d.x1=x1; d.y1=y1; d.score=s; d.class_id=0; return d;
}

TEST(ByteRepro, SteadyPersonProducesConfirmedTrack) {
  Tracker trk(orch_byte_cfg());
  trk.set_frame_size(640, 480);
  // A clear, large, centered person box, held steady for 6 frames.
  std::vector<TrackedBox> out;
  int max_people = 0;
  for (int f = 0; f < 6; ++f) {
    std::vector<Detection> dets = { person(250, 120, 400, 450, 0.9f) };
    out = trk.update(dets, f * 0.1);
    max_people = std::max<int>(max_people, (int)out.size());
    fprintf(stderr, "[ByteRepro] frame %d ts=%.1f -> tracks=%zu\n", f, f*0.1, out.size());
  }
  EXPECT_GT(max_people, 0) << "ByteTrack produced NO confirmed track for a steady person";
}

TEST(ByteRepro, LegacyControlProducesTrack) {
  TrackerConfig c = orch_byte_cfg();
  c.tracker_core = "legacy";  // control: legacy core on the same input
  Tracker trk(c);
  trk.set_frame_size(640, 480);
  std::vector<TrackedBox> out;
  int max_people = 0;
  for (int f = 0; f < 6; ++f) {
    std::vector<Detection> dets = { person(250, 120, 400, 450, 0.9f) };
    out = trk.update(dets, f * 0.1);
    max_people = std::max<int>(max_people, (int)out.size());
  }
  EXPECT_GT(max_people, 0) << "Legacy core produced NO track (control)";
}
