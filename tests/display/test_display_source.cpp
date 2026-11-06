// SPDX-License-Identifier: MIT
// Deterministic tests of DisplayFrameSource with a scripted FrameClient (no gRPC, no threads: poll_once_for_test()).
#include <gtest/gtest.h>

#include <functional>

#include "display/display_frame_source.h"

using namespace argus_display;

namespace {

// Smallest structurally valid JPEG for jpeg_info(): SOI, SOF0 (w x h), SOS header, EOI.
std::string tiny_jpeg(uint16_t w = 640, uint16_t h = 360, bool eoi = true) {
    std::string s = "\xFF\xD8";
    s += std::string("\xFF\xC0\x00\x0B\x08", 5) + char(h >> 8) + char(h & 0xFF) + char(w >> 8) + char(w & 0xFF) + std::string("\x01\x01\x11\x00", 4);
    s += std::string("\xFF\xDA\x00\x08\x01\x01\x00\x00\x3F\x00", 10);
    s += "data";
    if (eoi) s += "\xFF\xD9";
    return s;
}

struct Scripted : FrameClient {
    std::function<void(Health&)> on_health;       // fills the next health reply (called for every health())
    std::function<void(RawFrame&)> on_capture;
    int healths = 0, captures = 0, interests = 0;
    int64_t skew = 0;                              // daemon clock offset
    uint64_t freshness = 0;                        // current newest-frame receipt time
    int provider = 7;
    std::string boot = "boot-1";
    bool available = true;
    std::string code;
    bool health(Health& h) override {
        ++healths;
        h.rpc_ok = true; h.capture_available = available; h.error_code = code; h.boot_id = boot;
        h.daemon_mono_ms = static_cast<uint64_t>(mono_ms() + skew); h.freshness_mono_ms = freshness; h.provider = provider;
        h.width = 640; h.height = 360;
        if (on_health) on_health(h);
        return true;
    }
    bool capture(RawFrame& f) override {
        ++captures;
        f.ok = true; f.jpeg = tiny_jpeg(); f.width = 640; f.height = 360; f.provider = provider; f.mime = "image/jpeg";
        if (on_capture) on_capture(f);
        return f.ok;
    }
    void signal_interest() override { ++interests; }
    std::string describe() const override { return "scripted"; }
};

struct Rig {
    Scripted* c;
    std::unique_ptr<DisplayFrameSource> src;
    explicit Rig(DisplayCfg cfg = {}) {
        auto sc = std::make_unique<Scripted>();
        c = sc.get();
        cfg.enabled = true;
        src = std::make_unique<DisplayFrameSource>(cfg, std::move(sc));
    }
    void fresh(int64_t ago_ms = 20) { c->freshness = static_cast<uint64_t>(mono_ms() - ago_ms); }
};

}  // namespace

TEST(DisplaySource, JpegInfoRejectsIncompleteAndGarbage) {
    uint32_t w = 0, h = 0;
    EXPECT_TRUE(jpeg_info(tiny_jpeg(640, 360), w, h));
    EXPECT_EQ(w, 640u); EXPECT_EQ(h, 360u);
    EXPECT_FALSE(jpeg_info(tiny_jpeg(640, 360, false), w, h));        // no EOI: a torn / partial frame
    EXPECT_FALSE(jpeg_info("not a jpeg", w, h));
    EXPECT_FALSE(jpeg_info("", w, h));
}

TEST(DisplaySource, IdleMeansNoRpcAndNoInterest) {
    Rig r;
    r.fresh();
    // not touched, no hold: the loop would not poll; poll_once_for_test() polls regardless, so check the gate through status()
    EXPECT_EQ(r.src->status()["active"], false);
    EXPECT_EQ(r.c->healths, 0);
    r.src->touch_preview();
    EXPECT_EQ(r.src->status()["active"], true);
}

TEST(DisplaySource, AcceptsFreshFrameAndDerivesCaptureTimeFromFreshness) {
    Rig r;
    r.fresh(80);
    const int64_t before_utc = utc_ms();
    r.src->poll_once_for_test();
    DisplayFrame f; int64_t age = 0; std::string why;
    ASSERT_TRUE(r.src->latest(f, age, why)) << why;
    EXPECT_EQ(f.width, 640u); EXPECT_EQ(f.height, 360u);
    EXPECT_EQ(f.capture_mono_ms, static_cast<int64_t>(r.c->freshness));
    EXPECT_NEAR(static_cast<double>(f.capture_utc_ms), static_cast<double>(before_utc - 80), 40.0);   // converted with our own clock offset
    EXPECT_GE(f.uncertainty_ms, r.src->cfg().producer_latency_ms);
    EXPECT_EQ(r.c->interests, 1);
    EXPECT_EQ(r.c->captures, 1);
    EXPECT_EQ(r.c->healths, 2);                                                                       // before and after the capture
}

TEST(DisplaySource, SameFreshnessIsNotCapturedAgain) {
    Rig r;
    r.fresh();
    r.src->poll_once_for_test();
    r.src->poll_once_for_test();
    r.src->poll_once_for_test();
    EXPECT_EQ(r.c->captures, 1);                                                                      // duplicates cost a health call only
    EXPECT_EQ(r.src->status()["counters"]["accepted"], 1);
    EXPECT_EQ(r.src->status()["counters"]["same_frame_skipped"], 2);
}

TEST(DisplaySource, FrameThatChangedDuringTheCallIsDiscardedAsAmbiguous) {
    Rig r;
    r.fresh(40);
    int n = 0;
    r.c->on_health = [&](Health& h) { if (++n == 2) h.freshness_mono_ms += 200; };    // a newer frame arrived between the two health calls
    r.src->poll_once_for_test();
    DisplayFrame f; int64_t age; std::string why;
    EXPECT_FALSE(r.src->latest(f, age, why));
    EXPECT_EQ(r.src->status()["counters"]["ambiguous_discarded"], 1);
    r.c->on_health = nullptr;
    r.src->poll_once_for_test();                                                                      // the next cycle captures the new one cleanly
    EXPECT_TRUE(r.src->latest(f, age, why)) << why;
}

TEST(DisplaySource, StaleAndUnavailableAreReportedNotServed) {
    Rig r;
    r.fresh();
    r.c->available = false; r.c->code = "STALE";
    r.src->poll_once_for_test();
    EXPECT_EQ(r.src->status()["state"], "stale");
    EXPECT_EQ(r.src->status()["display_health"], "stale");
    EXPECT_EQ(r.c->captures, 0);
    r.c->code = "UNAVAILABLE";
    r.src->poll_once_for_test();
    EXPECT_EQ(r.src->status()["state"], "unavailable");
    DisplayFrame f; int64_t age; std::string why;
    EXPECT_FALSE(r.src->latest(f, age, why));
}

TEST(DisplaySource, ServiceClockThatIsNotOurMonotonicClockIsRejected) {
    Rig r;
    r.fresh();
    r.c->skew = 60000;                                                                                // e.g. a different time base
    r.src->poll_once_for_test();
    EXPECT_EQ(r.src->status()["state"], "clock_mismatch");
    EXPECT_EQ(r.c->captures, 0);
    EXPECT_EQ(r.src->status()["counters"]["clock_rejected"], 1);
}

TEST(DisplaySource, OnlyTheWestonWritebackProvenanceIsAccepted) {
    Rig r;
    r.fresh();
    r.c->provider = 5;                                                                                // SCREENSHOT provenance is not the display writeback
    r.src->poll_once_for_test();
    EXPECT_EQ(r.src->status()["state"], "provider_rejected");
    EXPECT_EQ(r.c->captures, 0);
}

TEST(DisplaySource, IncompleteOrOversizedJpegIsRejected) {
    Rig r;
    r.fresh();
    r.c->on_capture = [](RawFrame& f) { f.jpeg = tiny_jpeg(640, 360, false); };
    r.src->poll_once_for_test();
    EXPECT_EQ(r.src->status()["counters"]["invalid_jpeg"], 1);
    DisplayFrame f; int64_t age; std::string why;
    EXPECT_FALSE(r.src->latest(f, age, why));

    DisplayCfg cfg; cfg.max_jpeg_bytes = 8;
    Rig r2(cfg);
    r2.fresh();
    r2.src->poll_once_for_test();
    EXPECT_EQ(r2.src->status()["counters"]["oversize"], 1);
}

TEST(DisplaySource, OldFrameIsNotServedAsCurrent) {
    DisplayCfg cfg; cfg.max_age_ms = 300;
    Rig r(cfg);
    r.fresh(10);
    r.src->poll_once_for_test();
    DisplayFrame f; int64_t age; std::string why;
    ASSERT_TRUE(r.src->latest(f, age, why));
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    EXPECT_FALSE(r.src->latest(f, age, why));
    EXPECT_NE(why.find("old"), std::string::npos);
    EXPECT_EQ(r.src->status()["display_health"], "stale");
}

TEST(DisplaySource, ServiceRestartClearsTheRingAndIsCounted) {
    Rig r;
    r.fresh();
    r.src->poll_once_for_test();
    r.c->boot = "boot-2";                       // daemon restarted: monotonic base and frame numbering are new
    r.c->freshness += 500;
    r.src->poll_once_for_test();
    EXPECT_EQ(r.src->status()["counters"]["service_restarts"], 1);
    EXPECT_EQ(r.src->status()["ring"]["frames"], 1);
}

TEST(DisplaySource, RingIsBoundedAndNearestPicksTheClosestFrameInTheWindow) {
    DisplayCfg cfg; cfg.ring_frames = 4;
    Rig r(cfg);
    std::vector<int64_t> utc;
    for (int i = 0; i < 8; ++i) {
        r.c->freshness = static_cast<uint64_t>(mono_ms());
        r.src->poll_once_for_test();
        utc.push_back(utc_ms());
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    EXPECT_EQ(r.src->status()["ring"]["frames"], 4);                                                   // oldest dropped
    EXPECT_EQ(r.src->status()["counters"]["accepted"], 8);
    const auto hit = r.src->nearest(utc[6], 40, 40);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(static_cast<double>(hit->capture_utc_ms), static_cast<double>(utc[6]), 25.0);
    EXPECT_FALSE(r.src->nearest(utc[0], 10, 10).has_value());                                          // that frame was evicted
    EXPECT_FALSE(r.src->nearest(utc_ms() + 60000, 100, 100).has_value());
}

TEST(DisplaySource, ProofOfPlayHoldKeepsTheSourceActiveWithoutAViewer) {
    Rig r;
    EXPECT_EQ(r.src->status()["active"], false);
    r.src->hold("session-1");
    EXPECT_EQ(r.src->status()["active"], true);
    r.src->release("session-1");
    EXPECT_EQ(r.src->status()["active"], false);
}

TEST(DisplaySource, ThreadPollsOnlyWhileInterested) {
    DisplayCfg cfg; cfg.poll_ms = 50; cfg.linger_ms = 150;
    Rig r(cfg);
    r.fresh();
    r.src->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(r.c->healths, 0);                                                                       // nobody is looking: no RPC at all
    r.src->touch_preview();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int while_active = r.c->healths;
    EXPECT_GT(while_active, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));                                       // linger expired
    const int after = r.c->healths;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(r.c->healths, after);                                                                    // quiet again
    r.src->stop();
}
