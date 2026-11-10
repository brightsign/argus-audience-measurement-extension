// SPDX-License-Identifier: MIT
// D5 Proof of Play: pHash, verification decision logic, PopStore (reference library, events, evidence, retention).
#include "visualmemory/mock_embedding_provider.h"
#include "visualmemory/pop.h"
#include "jpeg_decoder.h"
#include "test_util.h"

#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>

using namespace visualmemory;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kT0 = 1'767'261'600'000LL;     // 2026-01-01 10:00:00 UTC

// Synthetic "ads": 160x100 pictures with distinct structure. kind 0: horizontal colour bands; 1: vertical bands; 2: diagonal.
std::vector<uint8_t> make_jpeg(int kind, int phase = 0, int w = 160, int h = 100) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        uint8_t* p = &rgb[(static_cast<size_t>(y) * w + x) * 3];
        const int xn = x * 160 / w, yn = y * 100 / h;                      // picture content independent of the pixel size
        int band = kind == 0 ? ((yn + phase) / 20) : kind == 1 ? ((xn + phase) / 20) : ((xn + yn + phase) / 30);
        static const uint8_t pal[6][3] = {{220, 40, 40}, {40, 200, 60}, {40, 60, 220}, {230, 220, 40}, {200, 40, 200}, {40, 210, 210}};
        const uint8_t* c = pal[(band + kind * 2) % 6];
        p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
    }
    std::vector<uint8_t> jpg; std::string err;
    EXPECT_TRUE(detail::encode_rgb_jpeg(rgb.data(), w, h, 92, jpg, err)) << err;
    return jpg;
}

// Smooth textured picture (like real footage, unlike the flat bands above); f1/f2 pick the pattern.
std::vector<uint8_t> make_smooth(double f1, double f2, int w, int h) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const double xn = x * 160.0 / w, yn = y * 100.0 / h;
        uint8_t* p = &rgb[(static_cast<size_t>(y) * w + x) * 3];
        p[0] = static_cast<uint8_t>(128 + 100 * std::sin(xn * f1)); p[1] = static_cast<uint8_t>(128 + 100 * std::sin(yn * f2 + 1));
        p[2] = static_cast<uint8_t>(128 + 100 * std::sin((xn + yn) * (f1 + f2) * 0.5 + 2));
    }
    std::vector<uint8_t> jpg; std::string err;
    EXPECT_TRUE(detail::encode_rgb_jpeg(rgb.data(), w, h, 92, jpg, err)) << err;
    return jpg;
}

struct Fx {
    vmtest::TempDir dir;
    MockEmbeddingProvider prov{64};
    PopStore store{[&] { PopConfig c; c.root_directory = dir.str(); return c; }()};
    Fx() { std::string e; EXPECT_TRUE(store.open(e)) << e; }
    void media(const std::string& id, int kind, int refs = 4) {
        MediaAsset a; a.media_id = id; a.uri = id + ".mp4"; a.slot_s = 30; a.duration_s = 60;
        std::string e; ASSERT_TRUE(store.register_media(a, e)) << e;
        for (int i = 0; i < refs; ++i) ASSERT_TRUE(store.add_reference(prov, id, i * 5000, make_jpeg(kind, i * 3), e)) << e;
    }
    void schedule(const std::string& sid, const std::string& expected, int captures = 3) {
        PopSession s; s.session_id = sid; s.expected_media_id = expected; s.slot_ms = 30000; s.captures_requested = captures;
        std::string e; ASSERT_TRUE(store.schedule_session(s, e)) << e;
    }
    void ev(const std::string& sid, const std::string& type, int64_t t, const std::string& reason = "", int64_t actual = -1, const std::string& played = "") {
        PopEventIn x; x.session_id = sid; x.type = type; x.utc_ms = kT0 + t; x.mono_ms = t; x.reason = reason; x.actual_play_ms = actual; x.played_media_id = played;
        std::string e; ASSERT_TRUE(store.apply_event(x, e)) << e;
    }
    void shot(const std::string& sid, int idx, int kind, int off, int phase = 0) {
        EvidenceIn in; in.session_id = sid; in.idx = idx; in.target_offset_ms = off; in.actual_offset_ms = off + 7; in.utc_ms = kT0 + 1000 + off;
        in.jpeg = make_jpeg(kind, phase); Evidence out; std::string e; ASSERT_TRUE(store.add_evidence(prov, in, out, e)) << e;
    }
    void failed_shot(const std::string& sid, int idx, int off) {
        EvidenceIn in; in.session_id = sid; in.idx = idx; in.status = "failed"; in.failure_reason = "Screenshot() returned false"; in.target_offset_ms = off;
        Evidence out; std::string e; ASSERT_TRUE(store.add_evidence(prov, in, out, e)) << e;
    }
    void full_play(const std::string& sid, int64_t actual = 30100, const std::string& reason = "slot_end") {
        ev(sid, "session_scheduled_run", 0); ev(sid, "play_requested", 10); ev(sid, "play_started", 300);
        ev(sid, "play_stopped", 30300, reason); ev(sid, "session_end", 30310, reason, actual);
    }
    Verification verify(const std::string& sid) {
        Verification v; std::string e; EXPECT_TRUE(store.verify_session(sid, VerifyThresholds{}, v, e)) << e; return v;
    }
};
} // namespace

TEST(PopHash, IdenticalSimilarAndDifferent) {
    uint64_t a = 0, a2 = 0, b = 0, c = 0; std::string e;
    ASSERT_TRUE(phash_of_jpeg(make_smooth(0.05, 0.09, 160, 100), a, e));
    ASSERT_TRUE(phash_of_jpeg(make_smooth(0.05, 0.09, 160, 100), a2, e));
    ASSERT_TRUE(phash_of_jpeg(make_smooth(0.05, 0.09, 320, 200), b, e));     // same picture at another resolution
    ASSERT_TRUE(phash_of_jpeg(make_smooth(0.13, 0.03, 160, 100), c, e));
    EXPECT_EQ(phash_distance(a, a2), 0);
    EXPECT_LE(phash_distance(a, b), 10) << "same content, other resolution";
    EXPECT_GT(phash_distance(a, c), phash_distance(a, b) + 6) << "different content is clearly farther";
    std::vector<uint8_t> junk{1, 2, 3};
    EXPECT_FALSE(phash_of_jpeg(junk, a, e));
}

TEST(PopVerifyLogic, DecisionTable) {
    auto unit = [](int n, int i) { std::vector<float> v(n, 0.0f); v[i] = 1.0f; return v; };
    VerifyInput base; base.expected_media_id = "A"; base.slot_ms = 30000; base.actual_play_ms = 30100; base.reported = true; base.ended = true; base.end_reason = "slot_end";
    base.refs = {{"A", 0, unit(8, 0), 0}, {"B", 0, unit(8, 1), 0}};
    auto ev = [&](int idx, int dim) { VerifyInput::Ev e; e.idx = idx; e.evidence_id = idx; e.actual_offset_ms = idx * 1000; e.emb = unit(8, dim); return e; };
    VerifyThresholds t;

    VerifyInput a = base; a.evidence = {ev(1, 0), ev(2, 0), ev(3, 0)};
    EXPECT_EQ(verify_playback(a, t).status, PopStatus::PlaybackCorroborated);

    VerifyInput b = base; b.evidence = {ev(1, 1), ev(2, 1), ev(3, 1)};
    Verification vb = verify_playback(b, t);
    EXPECT_EQ(vb.status, PopStatus::Mismatch); EXPECT_EQ(vb.best_media_id, "B");
    ASSERT_EQ(vb.frames.size(), 3u);
    EXPECT_EQ(vb.frames[0].best_other_media, "B"); EXPECT_EQ(vb.frames[0].best_other_ref_offset_ms, 0) << "the matched reference of the other advertisement is reported (diagnostic only)";
    EXPECT_NE(verification_to_json(vb).find("\"best_other_ref_offset_ms\":0"), std::string::npos);

    VerifyInput c = base; c.actual_play_ms = 9000; c.end_reason = "interrupted"; c.evidence = {ev(1, 0)};
    EXPECT_EQ(verify_playback(c, t).status, PopStatus::Incomplete) << "interrupted beats a visual match";

    VerifyInput d = base; d.captures_requested = 3; d.captures_failed = 3;
    EXPECT_EQ(verify_playback(d, t).status, PopStatus::CaptureUnsupported);

    VerifyInput e = base; e.reported = false; e.ended = false; e.actual_play_ms = -1;
    EXPECT_EQ(verify_playback(e, t).status, PopStatus::Scheduled);

    VerifyInput f = base; f.ended = false; f.end_reason.clear(); f.evidence = {ev(1, 0), ev(2, 0)};     // still running
    EXPECT_EQ(verify_playback(f, t).status, PopStatus::VisualMatch);

    VerifyInput g = base; g.evidence = {ev(1, 5), ev(2, 0)};                                              // one matches, one is unrelated: 50% < 60%
    Verification vg = verify_playback(g, t);
    EXPECT_EQ(vg.status, PopStatus::PlaybackReported) << vg.reason;

    VerifyInput h = base; h.captures_failed = 1; h.evidence = {ev(1, 0), ev(2, 0)};                      // partial capture loss keeps the verdict, noted in the reason
    Verification vh = verify_playback(h, t);
    EXPECT_EQ(vh.status, PopStatus::PlaybackCorroborated);
    EXPECT_NE(vh.reason.find("failed"), std::string::npos);

    VerifyInput i = base; i.refs = {{"B", 0, unit(8, 1), 0}}; i.evidence = {ev(1, 0)};                   // no reference frames of the expected media
    EXPECT_NE(verify_playback(i, t).status, PopStatus::PlaybackCorroborated);

    VerifyInput j = base; j.actual_play_ms = 20000;                                                       // 20 s of 30 s: below 90 %
    EXPECT_EQ(verify_playback(j, t).status, PopStatus::Incomplete);
}

TEST(PopStore, SchemaMigrationAndGuard) {
    vmtest::TempDir dir;
    { PopConfig c; c.root_directory = dir.str(); PopStore s(c); std::string e; ASSERT_TRUE(s.open(e)) << e; EXPECT_EQ(s.summary().schema_version, 2); }
    { PopConfig c; c.root_directory = dir.str(); PopStore s(c); std::string e; ASSERT_TRUE(s.open(e)) << e; EXPECT_EQ(s.summary().schema_version, 2); }   // idempotent reopen
    {
        sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open((dir.path() / "pop.db").c_str(), &db), SQLITE_OK);
        sqlite3_exec(db, "PRAGMA user_version=99;", nullptr, nullptr, nullptr); sqlite3_close(db);
    }
    PopConfig c; c.root_directory = dir.str(); PopStore s(c); std::string e;
    EXPECT_FALSE(s.open(e)); EXPECT_NE(e.find("newer"), std::string::npos);
}

TEST(PopStore, SchemaV1DatabaseIsUpgradedWithCaptureMethodColumns) {
    vmtest::TempDir dir;
    {   // a v1 database as written by the screenshot-only release: evidence table without the capture-method columns
        sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open((dir.path() / "pop.db").c_str(), &db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db,
            "CREATE TABLE display_capture_evidence(evidence_id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,idx INTEGER NOT NULL,"
            " target_offset_ms INTEGER NOT NULL,actual_offset_ms INTEGER NOT NULL,status TEXT NOT NULL,failure_reason TEXT NOT NULL DEFAULT '',"
            " source TEXT NOT NULL,path TEXT NOT NULL DEFAULT '',phash INTEGER NOT NULL DEFAULT 0,sha256 TEXT NOT NULL DEFAULT '',width INTEGER NOT NULL DEFAULT 0,"
            " height INTEGER NOT NULL DEFAULT 0,bytes INTEGER NOT NULL DEFAULT 0,took_ms INTEGER NOT NULL DEFAULT 0,utc_ms INTEGER NOT NULL,"
            " model_id INTEGER NOT NULL DEFAULT 0,embedding BLOB,embedded INTEGER NOT NULL DEFAULT 0,image_available INTEGER NOT NULL DEFAULT 0,"
            " created_at INTEGER NOT NULL,UNIQUE(session_id,idx));"
            "INSERT INTO display_capture_evidence(session_id,idx,target_offset_ms,actual_offset_ms,status,source,utc_ms,created_at) VALUES('old',1,3000,3007,'ok','main_display',1,1);"
            "PRAGMA user_version=1;", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }
    PopConfig c; c.root_directory = dir.str(); c.allow_schema_upgrade = true; PopStore s(c); std::string e;
    ASSERT_TRUE(s.open(e)) << e;
    EXPECT_EQ(s.summary().schema_version, 2);
    auto ev = s.list_evidence("old");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].capture_method, "brightscript_screenshot");      // old rows are screenshots by definition
    EXPECT_EQ(ev[0].uncertainty_ms, -1);
}

// An existing schema-1 pop.db must not be migrated or altered just because a newer build opens it.
TEST(PopStore, ExistingSchema1DatabaseIsLeftUntouchedAndStillWorksWithoutUpgrade) {
    vmtest::TempDir dir;
    const auto db_path = dir.path() / "pop.db";
    {   // a real v1 database: written by the schema-1 code path (an upgrade-allowed store is not used here; build it by hand)
        sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open(db_path.c_str(), &db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db,
            "CREATE TABLE pop_models(model_id INTEGER PRIMARY KEY,model_name TEXT NOT NULL,model_version TEXT NOT NULL,model_sha256 TEXT NOT NULL,preprocessing TEXT NOT NULL,dimension INTEGER NOT NULL,dtype TEXT NOT NULL,metric TEXT NOT NULL,UNIQUE(model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric));"
            "CREATE TABLE media_assets(media_id TEXT PRIMARY KEY,display_name TEXT NOT NULL,kind TEXT NOT NULL,uri TEXT NOT NULL,sha256 TEXT NOT NULL DEFAULT '',codec TEXT NOT NULL DEFAULT '',duration_s REAL NOT NULL DEFAULT 0,slot_s REAL NOT NULL DEFAULT 30,width INTEGER NOT NULL DEFAULT 0,height INTEGER NOT NULL DEFAULT 0,fps REAL NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
            "CREATE TABLE media_reference_frames(ref_id INTEGER PRIMARY KEY AUTOINCREMENT,media_id TEXT NOT NULL REFERENCES media_assets(media_id) ON DELETE CASCADE,offset_ms INTEGER NOT NULL,phash INTEGER NOT NULL,path TEXT NOT NULL,width INTEGER NOT NULL,height INTEGER NOT NULL,model_id INTEGER NOT NULL REFERENCES pop_models(model_id),embedding BLOB NOT NULL,created_at INTEGER NOT NULL,UNIQUE(media_id,offset_ms,model_id));"
            "CREATE TABLE playback_sessions(session_id TEXT PRIMARY KEY,job_id TEXT NOT NULL DEFAULT '',expected_media_id TEXT NOT NULL,played_media_id TEXT NOT NULL DEFAULT '',source TEXT NOT NULL DEFAULT 'main_display',state TEXT NOT NULL DEFAULT 'scheduled',end_reason TEXT NOT NULL DEFAULT '',scheduled_utc_ms INTEGER NOT NULL DEFAULT 0,start_utc_ms INTEGER NOT NULL DEFAULT 0,end_utc_ms INTEGER NOT NULL DEFAULT 0,actual_play_ms INTEGER NOT NULL DEFAULT -1,slot_ms INTEGER NOT NULL DEFAULT 30000,captures_requested INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL);"
            "CREATE TABLE playback_events(event_id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,type TEXT NOT NULL,mono_ms INTEGER NOT NULL,utc_ms INTEGER NOT NULL,detail_json TEXT NOT NULL,created_at INTEGER NOT NULL);"
            "CREATE TABLE display_capture_evidence(evidence_id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,idx INTEGER NOT NULL,target_offset_ms INTEGER NOT NULL,actual_offset_ms INTEGER NOT NULL,status TEXT NOT NULL,failure_reason TEXT NOT NULL DEFAULT '',source TEXT NOT NULL,path TEXT NOT NULL DEFAULT '',phash INTEGER NOT NULL DEFAULT 0,sha256 TEXT NOT NULL DEFAULT '',width INTEGER NOT NULL DEFAULT 0,height INTEGER NOT NULL DEFAULT 0,bytes INTEGER NOT NULL DEFAULT 0,took_ms INTEGER NOT NULL DEFAULT 0,utc_ms INTEGER NOT NULL,model_id INTEGER NOT NULL DEFAULT 0,embedding BLOB,embedded INTEGER NOT NULL DEFAULT 0,image_available INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,UNIQUE(session_id,idx));"
            "CREATE TABLE playback_verifications(verification_id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL UNIQUE,status TEXT NOT NULL,reason TEXT NOT NULL,expected_media_id TEXT NOT NULL,best_media_id TEXT NOT NULL,frames_total INTEGER NOT NULL,frames_ok INTEGER NOT NULL,frames_matched INTEGER NOT NULL,frames_mismatched INTEGER NOT NULL,mean_sim_expected REAL NOT NULL,min_sim_expected REAL NOT NULL,mean_phash_dist REAL NOT NULL,playback_reported INTEGER NOT NULL,playback_complete INTEGER NOT NULL,actual_play_ms INTEGER NOT NULL,slot_ms INTEGER NOT NULL,details_json TEXT NOT NULL,engine_version TEXT NOT NULL,created_at INTEGER NOT NULL);"
            "INSERT INTO playback_sessions(session_id,expected_media_id,created_at) VALUES('old','ad-a',1);"
            "INSERT INTO display_capture_evidence(session_id,idx,target_offset_ms,actual_offset_ms,status,source,utc_ms,created_at) VALUES('old',1,3000,3007,'failed','main_display',1,1);"
            "PRAGMA journal_mode=DELETE; PRAGMA user_version=1;", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }
    auto file_bytes = [&] { std::ifstream f(db_path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); };
    const std::string before = file_bytes();
    PopConfig c; c.root_directory = dir.str(); PopStore s(c); std::string e;      // allow_schema_upgrade is false by default
    ASSERT_TRUE(s.open(e)) << e;
    EXPECT_FALSE(s.schema_v2());
    EXPECT_EQ(s.summary().schema_version, 1);
    auto ev = s.list_evidence("old");                                            // reading works, rows are screenshots by definition
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].capture_method, "brightscript_screenshot");
    PopSession ps; EXPECT_TRUE(s.get_session("old", ps));
    s.close();
    EXPECT_EQ(file_bytes(), before) << "opening and reading a schema-1 pop.db must not change a single byte";
    // normal screenshot operation still works on the legacy file ...
    PopStore s2(c); ASSERT_TRUE(s2.open(e)) << e;
    MockEmbeddingProvider prov(64);
    EvidenceIn in; in.session_id = "old"; in.idx = 2; in.target_offset_ms = 15000; in.actual_offset_ms = 15007; in.utc_ms = kT0; in.jpeg = make_jpeg(0);
    Evidence out; EXPECT_TRUE(s2.add_evidence(prov, in, out, e)) << e;
    EXPECT_EQ(s2.summary().schema_version, 1);
    // ... but OS-rendered-frame evidence is refused (it needs the schema upgrade, which must be asked for explicitly)
    in.idx = 3; in.capture_method = "resilience_grpc"; in.uncertainty_ms = 250;
    EXPECT_FALSE(s2.add_evidence(prov, in, out, e));
    EXPECT_NE(e.find("allow_schema_upgrade"), std::string::npos) << e;
    Verification v; EXPECT_TRUE(s2.verify_session("old", VerifyThresholds{}, v, e)) << e;
}

TEST(PopStore, ReferenceLibraryRoundTripAndValidation) {
    Fx f; f.media("ad-a", 0, 5);
    MediaAsset m; ASSERT_TRUE(f.store.get_media("ad-a", m));
    EXPECT_EQ(m.reference_frames, 5u);
    EXPECT_EQ(f.store.list_references("ad-a").size(), 5u);
    auto refs = f.store.list_references("ad-a");
    std::vector<uint8_t> img; ASSERT_TRUE(f.store.reference_jpeg(refs[2].ref_id, img)); EXPECT_GT(img.size(), 100u);
    std::string e;
    MediaAsset bad; bad.media_id = "../x"; bad.uri = "u";
    EXPECT_FALSE(f.store.register_media(bad, e));
    bad.media_id = "ok"; bad.kind = "ftp"; EXPECT_FALSE(f.store.register_media(bad, e));
    EXPECT_FALSE(f.store.add_reference(f.prov, "missing", 0, make_jpeg(0), e)) << "unknown media";
    EXPECT_FALSE(f.store.add_reference(f.prov, "ad-a", 0, std::vector<uint8_t>{1, 2, 3}, e)) << "not a JPEG";
    EXPECT_TRUE(f.store.add_reference(f.prov, "ad-a", 0, make_jpeg(0), e)) << "same offset replaces, not duplicates";
    EXPECT_EQ(f.store.list_references("ad-a").size(), 5u);
    EXPECT_TRUE(f.store.clear_references("ad-a"));
    EXPECT_TRUE(f.store.list_references("ad-a").empty());
    EXPECT_FALSE(fs::exists(f.dir.path() / "pop" / "refs" / "ad-a"));
    MediaAsset keep; EXPECT_TRUE(f.store.get_media("ad-a", keep)) << "the asset itself stays";
    EXPECT_TRUE(f.store.delete_media("ad-a"));
    EXPECT_TRUE(f.store.list_references("ad-a").empty());
    EXPECT_FALSE(fs::exists(f.dir.path() / "pop" / "refs" / "ad-a"));
}

TEST(PopStore, ScheduledThenPlaybackCorroborated) {                       // Test A
    Fx f; f.media("ad-a", 0); f.media("ad-b", 1);
    f.schedule("s1", "ad-a");
    EXPECT_EQ(f.verify("s1").status, PopStatus::Scheduled);
    f.full_play("s1", 30105);
    f.shot("s1", 1, 0, 3000, 1); f.shot("s1", 2, 0, 15000, 2); f.shot("s1", 3, 0, 27000, 3);   // the display shows ad A
    Verification v = f.verify("s1");
    EXPECT_EQ(v.status, PopStatus::PlaybackCorroborated) << v.reason;
    EXPECT_EQ(v.frames_ok, 3); EXPECT_EQ(v.frames_matched, 3);
    PopSession s; ASSERT_TRUE(f.store.get_session("s1", s));
    EXPECT_EQ(s.state, "ended"); EXPECT_EQ(s.actual_play_ms, 30105); EXPECT_EQ(s.end_reason, "slot_end"); EXPECT_GT(s.start_utc_ms, 0);
    EXPECT_EQ(f.store.list_events("s1").size(), 5u);
    Verification stored; ASSERT_TRUE(f.store.get_verification("s1", stored));
    EXPECT_EQ(stored.status, PopStatus::PlaybackCorroborated);
    EXPECT_NE(f.store.get_verification_json("s1").find("\"frames\":["), std::string::npos);
}

TEST(PopStore, WrongAdIsMismatch) {                                        // Test B
    Fx f; f.media("ad-a", 0); f.media("ad-b", 1);
    f.schedule("s2", "ad-a");
    f.full_play("s2");
    f.shot("s2", 1, 1, 3000); f.shot("s2", 2, 1, 15000, 2); f.shot("s2", 3, 1, 27000, 3);     // the display shows ad B
    Verification v = f.verify("s2");
    EXPECT_EQ(v.status, PopStatus::Mismatch) << v.reason;
    EXPECT_EQ(v.best_media_id, "ad-b");
    EXPECT_NE(v.reason.find("ad-b"), std::string::npos);
}

TEST(PopStore, InterruptedIsIncomplete) {                                  // Test C
    Fx f; f.media("ad-a", 0);
    f.schedule("s3", "ad-a");
    f.ev("s3", "session_scheduled_run", 0); f.ev("s3", "play_started", 300);
    f.ev("s3", "play_stopped", 10300, "interrupted"); f.ev("s3", "session_end", 10310, "interrupted", 10010);
    f.shot("s3", 1, 0, 3000);
    Verification v = f.verify("s3");
    EXPECT_EQ(v.status, PopStatus::Incomplete) << v.reason;
    EXPECT_FALSE(v.playback_complete);
}

TEST(PopStore, ScreenshotFailureIsCaptureUnsupported) {                    // Test D
    Fx f; f.media("ad-a", 0);
    f.schedule("s4", "ad-a");
    f.full_play("s4");
    f.failed_shot("s4", 1, 3000); f.failed_shot("s4", 2, 15000); f.failed_shot("s4", 3, 27000);
    Verification v = f.verify("s4");
    EXPECT_EQ(v.status, PopStatus::CaptureUnsupported) << v.reason;
    EXPECT_TRUE(v.playback_reported);
    auto ev = f.store.list_evidence("s4"); ASSERT_EQ(ev.size(), 3u);
    EXPECT_EQ(ev[0].status, "failed"); EXPECT_TRUE(ev[0].path.empty());
}

TEST(PopStore, EvidenceIsPartitionedHourlyAndRetentionKeepsScores) {
    Fx f; f.media("ad-a", 0);
    f.schedule("s5", "ad-a"); f.full_play("s5");
    f.shot("s5", 1, 0, 3000); f.shot("s5", 2, 0, 15000);
    auto evs = f.store.list_evidence("s5"); ASSERT_EQ(evs.size(), 2u);
    EXPECT_EQ(evs[0].path.rfind("pop/evidence/2026/01/01/10/main_display/", 0), 0u) << evs[0].path;
    EXPECT_TRUE(fs::exists(f.dir.path() / evs[0].path));
    EXPECT_EQ(PopStore::partition_path("a b/c", kT0, "x.jpg"), "pop/evidence/2026/01/01/10/a_b_c/x.jpg");
    EXPECT_EQ(PopStore::partition_path("cam", kT0 + 3600 * 1000, "x.jpg"), "pop/evidence/2026/01/01/11/cam/x.jpg");
    PopSummary before = f.store.summary(); EXPECT_GT(before.evidence_bytes, 0u);
    std::vector<uint8_t> img; EXPECT_TRUE(f.store.evidence_jpeg(evs[0].evidence_id, img));
    EXPECT_EQ(f.store.prune_evidence(kT0 + 1000 + 10000, 0), 1u) << "only the 3 s capture is older than the cut-off";
    EXPECT_FALSE(fs::exists(f.dir.path() / evs[0].path)); EXPECT_TRUE(fs::exists(f.dir.path() / evs[1].path));
    EXPECT_FALSE(f.store.evidence_jpeg(evs[0].evidence_id, img));
    EXPECT_EQ(f.store.prune_evidence(0, 1), 1u) << "byte budget removes the oldest remaining";
    EXPECT_EQ(f.store.summary().evidence_bytes, 0u);
    EXPECT_EQ(f.store.summary().evidence, 2u) << "rows and scores are kept";
    EXPECT_EQ(f.verify("s5").frames_ok, 2) << "verification still works from the stored embeddings";
}

TEST(PopStore, SessionsAdHocDuplicatesFilterAndPersistence) {
    vmtest::TempDir dir;
    {
        PopConfig c; c.root_directory = dir.str(); PopStore s(c); std::string e; ASSERT_TRUE(s.open(e));
        PopSession p; p.session_id = "x1"; p.expected_media_id = "ad-a"; ASSERT_TRUE(s.schedule_session(p, e));
        EXPECT_FALSE(s.schedule_session(p, e)) << "duplicate id refused";
        p.session_id = "bad id"; EXPECT_FALSE(s.schedule_session(p, e));
        PopEventIn ei; ei.session_id = "adhoc"; ei.type = "play_started"; ei.utc_ms = kT0; ei.expected_media_id = "ad-b"; ASSERT_TRUE(s.apply_event(ei, e)) << e;
        PopSession q; ASSERT_TRUE(s.get_session("adhoc", q)); EXPECT_EQ(q.expected_media_id, "ad-b"); EXPECT_EQ(q.state, "running");
        ei.type = ""; EXPECT_FALSE(s.apply_event(ei, e));
    }
    PopConfig c; c.root_directory = dir.str(); PopStore s(c); std::string e; ASSERT_TRUE(s.open(e));
    uint64_t total = 0;
    EXPECT_EQ(s.list_sessions(SessionFilter{}, 10, 0, &total).size(), 2u); EXPECT_EQ(total, 2u);
    SessionFilter flt; flt.expected_media_id = "ad-b"; EXPECT_EQ(s.list_sessions(flt, 10, 0).size(), 1u);
    flt = SessionFilter{}; flt.status = "scheduled"; EXPECT_EQ(s.list_sessions(flt, 10, 0).size(), 2u) << "no verification yet counts as scheduled";
}

// ---- OS rendered-frame evidence (capture_method resilience_grpc) -----------------------------------------------------------
namespace {
void shot_via(Fx& f, const std::string& sid, int idx, int kind, int off, const std::string& method, int phase = 0, int uncertainty = 300) {
    EvidenceIn in; in.session_id = sid; in.idx = idx; in.target_offset_ms = off; in.actual_offset_ms = off + 40; in.utc_ms = kT0 + 1000 + off;
    in.capture_method = method; in.provenance = method == "resilience_grpc" ? "weston_vop2_writeback (OS provider 7)" : ""; in.uncertainty_ms = method == "resilience_grpc" ? uncertainty : -1;
    in.jpeg = make_jpeg(kind, phase); Evidence out; std::string e; ASSERT_TRUE(f.store.add_evidence(f.prov, in, out, e)) << e;
}
void display_refs(Fx& f, const std::string& id, int kind) {          // references in the letterboxed display geometry live in a hidden profile row
    MediaAsset base; ASSERT_TRUE(f.store.get_media(id, base));
    MediaAsset d = base; d.media_id = id + kDisplayProfileSuffix();
    std::string e; ASSERT_TRUE(f.store.register_media(d, e)) << e;
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(f.store.add_reference(f.prov, d.media_id, i * 5000, make_jpeg(kind, i * 3, 160, 90), e)) << e;
}
}  // namespace

TEST(PopStore, EvidenceRecordsHowItWasCapturedAndItsTimeUncertainty) {
    Fx f; f.media("ad-a", 0);
    f.schedule("g1", "ad-a"); f.full_play("g1");
    shot_via(f, "g1", 1, 0, 3000, "resilience_grpc", 1, 275);
    shot_via(f, "g1", 2, 0, 15000, "brightscript_screenshot", 2);
    auto ev = f.store.list_evidence("g1");
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].capture_method, "resilience_grpc");
    EXPECT_NE(ev[0].provenance.find("weston_vop2_writeback"), std::string::npos);
    EXPECT_EQ(ev[0].uncertainty_ms, 275);
    EXPECT_EQ(ev[1].capture_method, "brightscript_screenshot");
    EXPECT_EQ(ev[1].uncertainty_ms, -1);
}

TEST(PopStore, DisplayEvidenceIsComparedOnlyWithDisplayGeometryReferences) {
    Fx f; f.media("ad-a", 0); f.media("ad-b", 1);
    display_refs(f, "ad-a", 2);                                     // the display geometry of ad A looks like kind 2 in this synthetic set
    display_refs(f, "ad-b", 1);
    // hidden rows are not listed as advertisements, but can be read directly
    for (const auto& m : f.store.list_media()) EXPECT_EQ(m.media_id.find(kDisplayProfileSuffix()), std::string::npos) << m.media_id;
    MediaAsset hidden; EXPECT_TRUE(f.store.get_media("ad-a" + std::string(kDisplayProfileSuffix()), hidden)); EXPECT_EQ(hidden.reference_frames, 4u);

    // Evidence captured from the OS frames showing ad A (kind 2 in display geometry): corroborated against the display references,
    f.schedule("g2", "ad-a"); f.full_play("g2");
    for (int i = 0; i < 3; ++i) shot_via(f, "g2", i + 1, 2, 3000 + i * 12000, "resilience_grpc", i);
    Verification v = f.verify("g2");
    EXPECT_EQ(v.status, PopStatus::PlaybackCorroborated) << v.reason;
    EXPECT_EQ(v.best_media_id, "ad-a");
    // ... while the SAME pictures taken through the screenshot path are compared with the screenshot references (kind 0) and do not match.
    f.schedule("g3", "ad-a"); f.full_play("g3");
    for (int i = 0; i < 3; ++i) shot_via(f, "g3", i + 1, 2, 3000 + i * 12000, "brightscript_screenshot", i);
    EXPECT_NE(f.verify("g3").status, PopStatus::PlaybackCorroborated);
}

TEST(PopStore, DisplayEvidenceFallsBackToScreenshotReferencesWhenNoDisplaySetExists) {
    Fx f; f.media("ad-a", 0); f.media("ad-b", 1);                   // no display_letterbox references at all
    f.schedule("g4", "ad-a"); f.full_play("g4");
    for (int i = 0; i < 3; ++i) shot_via(f, "g4", i + 1, 0, 3000 + i * 12000, "resilience_grpc", i);
    EXPECT_EQ(f.verify("g4").status, PopStatus::PlaybackCorroborated);
}

TEST(PopStore, DisplayWrongAdMissingAndInterruptedOutcomesAreUnchanged) {
    Fx f; f.media("ad-a", 0); f.media("ad-b", 1); display_refs(f, "ad-a", 2); display_refs(f, "ad-b", 1);
    f.schedule("w", "ad-a"); f.full_play("w");
    for (int i = 0; i < 3; ++i) shot_via(f, "w", i + 1, 1, 3000 + i * 12000, "resilience_grpc", i);   // display shows ad B
    Verification vb = f.verify("w");
    EXPECT_EQ(vb.status, PopStatus::Mismatch) << vb.reason;
    EXPECT_EQ(vb.best_media_id, "ad-b");

    f.schedule("m", "ad-a"); f.full_play("m");                                                          // every display capture failed
    for (int i = 0; i < 3; ++i) { EvidenceIn in; in.session_id = "m"; in.idx = i + 1; in.status = "failed"; in.capture_method = "resilience_grpc"; in.failure_reason = "no fresh display frame"; Evidence o; std::string e; ASSERT_TRUE(f.store.add_evidence(f.prov, in, o, e)) << e; }
    EXPECT_EQ(f.verify("m").status, PopStatus::CaptureUnsupported);

    f.schedule("i", "ad-a");                                                                            // interrupted at 10 s
    f.ev("i", "session_scheduled_run", 0); f.ev("i", "play_started", 300); f.ev("i", "play_stopped", 10300, "interrupted"); f.ev("i", "session_end", 10310, "interrupted", 10000);
    shot_via(f, "i", 1, 2, 3000, "resilience_grpc");
    EXPECT_EQ(f.verify("i").status, PopStatus::Incomplete);
}

// ---- display-evidence search (Visual Search, source "BrightSign Display") ----------------------------------------------------------------------
namespace {
// Three sessions on the display path: g1 and g2 show ad A (kind 0), g3 shows ad B (kind 1); plus one screenshot-path row.
void search_fixture(Fx& f) {
    f.media("ad-a", 0); f.media("ad-b", 1);
    f.schedule("g1", "ad-a"); f.full_play("g1");
    shot_via(f, "g1", 1, 0, 3000, "resilience_grpc"); shot_via(f, "g1", 2, 0, 15000, "resilience_grpc", 3); shot_via(f, "g1", 3, 0, 27000, "resilience_grpc", 6);
    f.schedule("g2", "ad-a"); f.full_play("g2");
    shot_via(f, "g2", 1, 0, 3000, "resilience_grpc");
    f.schedule("g3", "ad-b"); f.full_play("g3");
    shot_via(f, "g3", 1, 1, 3000, "resilience_grpc"); shot_via(f, "g3", 2, 1, 15000, "resilience_grpc");
    f.schedule("s1", "ad-a"); f.full_play("s1");
    shot_via(f, "s1", 1, 0, 3000, "brightscript_screenshot");
}
}

TEST(PopSearch, FindsTheMatchingEvidenceRankedBySimilarityWithItsSessionAndTime) {
    Fx f; search_fixture(f);
    EvidenceSearchOptions o; o.top_k = 10;
    EvidenceSearchResult r; std::string e;
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)) << e;
    ASSERT_FALSE(r.hits.empty());
    EXPECT_EQ(r.scanned, 6u);                                           // only the resilience_grpc rows are candidates (not the screenshot)
    EXPECT_EQ(r.coverage.rows_searchable, 6u);
    EXPECT_EQ(r.coverage.sessions, 3u);
    EXPECT_EQ(r.coverage.images_retained, 6u);
    for (size_t i = 1; i < r.hits.size(); ++i) EXPECT_GE(r.hits[i - 1].similarity, r.hits[i].similarity);
    EXPECT_NEAR(r.hits[0].similarity, 1.0, 1e-3);                      // the identical picture
    EXPECT_EQ(r.hits[0].evidence.capture_method, "resilience_grpc");
    for (const auto& h : r.hits) { EXPECT_GE(h.similarity, 0.80f); EXPECT_NE(h.evidence.session_id, "g3"); EXPECT_NE(h.evidence.session_id, "s1"); }
    EXPECT_EQ(r.hits[0].evidence.utc_ms, kT0 + 1000 + r.hits[0].evidence.target_offset_ms);   // time comes from the stored row
    EXPECT_NE(r.hits[0].evidence.uncertainty_ms, -1);
}

TEST(PopSearch, NoMatchIsReportedAsNoMatchButTheClosestCandidateIsStillShown) {
    Fx f; search_fixture(f);
    EvidenceSearchOptions o; o.min_similarity = 0.999f;
    EvidenceSearchResult r; std::string e;
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_smooth(0.31, 0.17, 160, 100), o, r, e)) << e;      // a picture that was never on the display
    EXPECT_TRUE(r.hits.empty());
    ASSERT_TRUE(r.has_closest);
    EXPECT_LT(r.closest.similarity, 0.999f);
    EXPECT_GT(r.scanned, 0u);
}

TEST(PopSearch, FiltersByTimeAdvertisementCaptureMethodAndImageAvailability) {
    Fx f; search_fixture(f);
    EvidenceSearchResult r; std::string e; EvidenceSearchOptions o; o.has_min_similarity = false; o.top_k = 50;
    o.media_id = "ad-b";
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 2u); for (const auto& h : r.hits) EXPECT_EQ(h.evidence.session_id, "g3");
    o.media_id.clear(); o.start_utc_ms = kT0 + 1000 + 10000; o.end_utc_ms = kT0 + 1000 + 20000;       // only the 15 s captures
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 2u); for (const auto& h : r.hits) EXPECT_EQ(h.evidence.target_offset_ms, 15000);
    o.start_utc_ms = o.end_utc_ms = 0; o.capture_method = "brightscript_screenshot";
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 1u); EXPECT_EQ(r.hits[0].evidence.session_id, "s1");
    o.capture_method.clear();                                                                           // any display capture
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 7u);
    // retention removes the JPEG but keeps the row and the vector: still searchable, but require_image drops it
    f.store.prune_evidence(kT0 + 1000 + 5000, 0);
    o.capture_method = "resilience_grpc"; o.require_image = false;
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 6u); EXPECT_LT(r.coverage.images_retained, 6u);
    o.require_image = true;
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)) << e;
    EXPECT_LT(r.scanned, 6u);
}

TEST(PopSearch, EmptyStoreFailedCapturesAndOtherModelsGiveNoCandidatesNotErrors) {
    Fx f; f.media("ad-a", 0);
    EvidenceSearchResult r; std::string e; EvidenceSearchOptions o;
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)) << e;
    EXPECT_TRUE(r.hits.empty()); EXPECT_FALSE(r.has_closest); EXPECT_EQ(r.scanned, 0u); EXPECT_EQ(r.coverage.rows_searchable, 0u);
    f.schedule("m", "ad-a"); f.full_play("m");
    EvidenceIn in; in.session_id = "m"; in.idx = 1; in.status = "failed"; in.capture_method = "resilience_grpc"; in.failure_reason = "no fresh display frame";
    Evidence ev; ASSERT_TRUE(f.store.add_evidence(f.prov, in, ev, e)) << e;
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 0u); EXPECT_EQ(r.coverage.rows_total, 1u);
    // evidence stored with a 64-d model is not comparable with a 32-d query: reported as another model, never mixed
    shot_via(f, "m", 2, 0, 3000, "resilience_grpc");
    MockEmbeddingProvider other(32);
    ASSERT_TRUE(f.store.search_evidence(other, make_jpeg(0, 0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 0u); EXPECT_EQ(r.coverage.rows_other_model, 1u); EXPECT_TRUE(r.hits.empty());
}

TEST(PopSearch, RejectsBadArgumentsAndBadImages) {
    Fx f; search_fixture(f);
    EvidenceSearchResult r; std::string e; EvidenceSearchOptions o;
    o.top_k = 0; EXPECT_FALSE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)); o.top_k = 51; EXPECT_FALSE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e));
    o.top_k = 5; o.min_similarity = 2.f; EXPECT_FALSE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e));
    o.min_similarity = 0.8f; EXPECT_FALSE(f.store.search_evidence(f.prov, {}, o, r, e));
    EXPECT_FALSE(f.store.search_evidence(f.prov, std::vector<uint8_t>(100, 0x42), o, r, e));
    EXPECT_FALSE(e.empty());
}

TEST(PopSearch, CancellationStopsTheScan) {
    Fx f; search_fixture(f);
    EvidenceSearchResult r; std::string e; EvidenceSearchOptions o; o.cancel = [] { return true; };
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 0), o, r, e)) << e;
    EXPECT_TRUE(r.cancelled); EXPECT_EQ(r.scanned, 0u);
}

// ---- display geometry transform for uploaded references -------------------------------------------------------------------------------------
TEST(PopSearch, DisplayGeometryMatchesTheValidatedLetterboxTransform) {
    // 1920x1080 test picture: left of x=960 red else green; above y=540 blue-ish dominant channel else yellow-ish, so each quadrant is identifiable.
    const int W = 1920, H = 1080;
    std::vector<uint8_t> rgb(static_cast<size_t>(W) * H * 3);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        uint8_t* p = &rgb[(static_cast<size_t>(y) * W + x) * 3];
        const bool left = x < 960, top = y < 540;
        p[0] = left ? 220 : 20; p[1] = left ? 20 : 220; p[2] = top ? 200 : 30;
    }
    std::vector<uint8_t> in, out; std::string e; ASSERT_TRUE(detail::encode_rgb_jpeg(rgb.data(), W, H, 95, in, e)) << e;
    int sw = 0, sh = 0; ASSERT_TRUE(display_letterbox_jpeg(in, out, sw, sh, e)) << e;
    EXPECT_EQ(sw, W); EXPECT_EQ(sh, H);
    detail::DecodedImage d; ASSERT_TRUE(detail::decode_jpeg_rgb(out.data(), out.size(), d, e)) << e;
    ASSERT_EQ(d.width, 640); ASSERT_EQ(d.height, 360);
    auto at = [&](int x, int y, int c) { return static_cast<int>(d.rgb[(static_cast<size_t>(y) * 640 + x) * 3 + c]); };
    for (int y : {5, 180, 355}) { EXPECT_LT(at(8, y, 0), 12); EXPECT_LT(at(630, y, 1), 12); }       // black side bars (x<32 and x>=608)
    // The crop keeps the left 1280 of 1920 px: red/green boundary at 960/1280 of the 576 px picture = x 32+432 = 464; the top 800 of 1080 rows:
    // the blue/low boundary at 540/800 of 360 rows = y 243.
    EXPECT_GT(at(200, 100, 0), 150); EXPECT_LT(at(200, 100, 1), 80); EXPECT_GT(at(200, 100, 2), 140);  // red, blue-ish
    EXPECT_GT(at(560, 100, 1), 150); EXPECT_LT(at(560, 100, 0), 80);                                  // green (right of x=464)
    EXPECT_LT(at(200, 300, 2), 90);                                                                   // below y=243: low blue
    EXPECT_GT(at(440, 100, 0), 150); EXPECT_GT(at(490, 100, 1), 150);                                 // boundary within a few px of 464
}

TEST(PopSearch, DisplayGeometryRejectsNonJpegAndHandlesTinyImages) {
    std::vector<uint8_t> out; int w, h; std::string e;
    EXPECT_FALSE(display_letterbox_jpeg({1, 2, 3, 4}, out, w, h, e));
    std::vector<uint8_t> px(4 * 3 * 3, 128), tiny; ASSERT_TRUE(detail::encode_rgb_jpeg(px.data(), 4, 3, 90, tiny, e)) << e;
    ASSERT_TRUE(display_letterbox_jpeg(tiny, out, w, h, e)) << e;
    detail::DecodedImage d; ASSERT_TRUE(detail::decode_jpeg_rgb(out.data(), out.size(), d, e)) << e;
    EXPECT_EQ(d.width, 640); EXPECT_EQ(d.height, 360);
}

// ---- Phase 2: display history (about one display frame per second, separate from the three verification checkpoints) ---------------------------
namespace {
HistoryFrameIn hist_in(const std::string& sid, int k, int kind, int phase = 0, int64_t utc = 0) {
    HistoryFrameIn h; h.session_id = sid; h.sec_idx = k; h.target_offset_ms = 500 + k * 1000; h.actual_offset_ms = 520 + k * 1000;
    h.utc_ms = utc ? utc : kT0 + 500 + k * 1000; h.mono_ms = 1000 + k * 1000; h.uncertainty_ms = 252; h.expected_media_id = "ad-a";
    h.provenance = "weston_vop2_writeback p7"; h.jpeg = make_jpeg(kind, phase); return h;
}
int64_t user_version_of(const std::string& path) {
    sqlite3* db = nullptr; int64_t v = -1;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(db); return v;
}
bool has_table(const std::string& path, const char* name) {
    sqlite3* db = nullptr; bool r = false;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", -1, &s, nullptr) == SQLITE_OK) { sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC); r = sqlite3_step(s) == SQLITE_ROW; }
        sqlite3_finalize(s);
    }
    sqlite3_close(db); return r;
}
void history_fixture(Fx& f) {            // two ad-A sessions with 20 samples each (different phases), one ad-B session with 10
    f.media("ad-a", 0); f.media("ad-b", 1);
    std::string e; ASSERT_TRUE(f.store.history_open(e)) << e;
    HistoryLimits lim;
    for (const char* sid : {"h1", "h2"}) {
        f.schedule(sid, "ad-a"); f.full_play(sid);
        for (int k = 0; k < 20; ++k) { HistoryFrame o; ASSERT_TRUE(f.store.add_history_frame(hist_in(sid, k, 0, k * 4, std::string(sid) == "h1" ? 0 : kT0 + 600000 + 500 + k * 1000), lim, o, e)) << e; }
    }
    f.schedule("h3", "ad-b"); f.full_play("h3");
    for (int k = 0; k < 10; ++k) { HistoryFrame o; ASSERT_TRUE(f.store.add_history_frame(hist_in("h3", k, 1, k * 4, kT0 + 1200000 + 500 + k * 1000), lim, o, e)) << e; }
    while (true) { const int n = f.store.embed_pending_history(f.prov, 100, e); ASSERT_GE(n, 0) << e; if (n == 0) break; }
}
}  // namespace

TEST(PopHistory, TheTableIsAdditiveCreatedOnDemandAndSchemaVersionIsUnchanged) {
    Fx f;
    const std::string db = (fs::path(f.dir.str()) / "pop.db").string();
    EXPECT_FALSE(f.store.history_ready());
    EXPECT_FALSE(has_table(db, "display_history_frames")) << "opening a store creates nothing";
    HistoryFrame o; HistoryLimits lim; std::string e;
    EXPECT_FALSE(f.store.add_history_frame(hist_in("x", 0, 0), lim, o, e)) << "refused while history is not enabled";
    ASSERT_TRUE(f.store.history_open(e)) << e;
    EXPECT_TRUE(f.store.history_ready());
    EXPECT_TRUE(has_table(db, "display_history_frames"));
    EXPECT_EQ(user_version_of(db), 2) << "user_version stays 2: an older binary keeps opening the file";
    EXPECT_TRUE(f.store.history_open(e)) << "idempotent";
}

TEST(PopHistory, ALegacySchema1FileIsNeverTouchedByHistory) {
    vmtest::TempDir dir; const std::string path = (fs::path(dir.str()) / "pop.db").string();
    {   // a real schema-1 file (as written by the first D5 release)
        sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        const char* ddl = "CREATE TABLE playback_sessions(session_id TEXT PRIMARY KEY,job_id TEXT NOT NULL DEFAULT '',expected_media_id TEXT NOT NULL,played_media_id TEXT NOT NULL DEFAULT '',"
            "source TEXT NOT NULL DEFAULT 'main_display',state TEXT NOT NULL DEFAULT 'scheduled',end_reason TEXT NOT NULL DEFAULT '',scheduled_utc_ms INTEGER NOT NULL DEFAULT 0,"
            "start_utc_ms INTEGER NOT NULL DEFAULT 0,end_utc_ms INTEGER NOT NULL DEFAULT 0,actual_play_ms INTEGER NOT NULL DEFAULT -1,slot_ms INTEGER NOT NULL DEFAULT 30000,"
            "captures_requested INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL);"
            "CREATE TABLE display_capture_evidence(evidence_id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,idx INTEGER NOT NULL,target_offset_ms INTEGER NOT NULL,"
            "actual_offset_ms INTEGER NOT NULL,status TEXT NOT NULL,failure_reason TEXT NOT NULL DEFAULT '',source TEXT NOT NULL,path TEXT NOT NULL DEFAULT '',phash INTEGER NOT NULL DEFAULT 0,"
            "sha256 TEXT NOT NULL DEFAULT '',width INTEGER NOT NULL DEFAULT 0,height INTEGER NOT NULL DEFAULT 0,bytes INTEGER NOT NULL DEFAULT 0,took_ms INTEGER NOT NULL DEFAULT 0,"
            "utc_ms INTEGER NOT NULL,model_id INTEGER NOT NULL DEFAULT 0,embedding BLOB,embedded INTEGER NOT NULL DEFAULT 0,image_available INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,"
            "UNIQUE(session_id,idx)); PRAGMA user_version=1;";
        ASSERT_EQ(sqlite3_exec(db, ddl, nullptr, nullptr, nullptr), SQLITE_OK); sqlite3_close(db);
    }
    std::ifstream a(path, std::ios::binary); std::string before((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>()); a.close();
    PopConfig c; c.root_directory = dir.str(); PopStore s(c); std::string e; ASSERT_TRUE(s.open(e)) << e;
    EXPECT_FALSE(s.history_open(e)) << "history needs a schema-2 file; the legacy file is not upgraded";
    EXPECT_FALSE(s.history_ready()); s.close();
    std::ifstream b(path, std::ios::binary); std::string after((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
    EXPECT_EQ(before, after) << "byte-identical";
}

TEST(PopHistory, StoresTheOriginalJpegBytesAndEmbedsThemAsynchronously) {
    Fx f; f.schedule("s", "ad-a"); f.full_play("s");
    std::string e; ASSERT_TRUE(f.store.history_open(e)) << e;
    HistoryLimits lim; HistoryFrame o; const auto in = hist_in("s", 0, 0);
    ASSERT_TRUE(f.store.add_history_frame(in, lim, o, e)) << e;
    EXPECT_EQ(o.role, "history_sample"); EXPECT_TRUE(o.image_available); EXPECT_FALSE(o.embedded) << "the vector comes later";
    EXPECT_EQ(o.width, 160); EXPECT_EQ(o.height, 100); EXPECT_NE(o.path.find("pop/history/2026/01/01/10/"), std::string::npos) << o.path;
    std::ifstream fin(fs::path(f.dir.str()) / o.path, std::ios::binary); std::vector<uint8_t> disk((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
    EXPECT_EQ(disk, in.jpeg) << "the compressed bytes are stored unchanged (no decode/re-encode)";
    EXPECT_EQ(f.store.history_stats().pending_embed, 1u);
    EvidenceSearchResult r; EvidenceSearchOptions so; so.has_min_similarity = false;
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0), so, r, e)) << e;
    EXPECT_EQ(r.scanned, 0u) << "an un-embedded sample is not searchable yet";
    EXPECT_EQ(f.store.embed_pending_history(f.prov, 10, e), 1) << e;
    EXPECT_EQ(f.store.history_stats().pending_embed, 0u);
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0), so, r, e)) << e;
    ASSERT_EQ(r.hits.size(), 1u);
    EXPECT_EQ(r.hits[0].evidence.role, "history_sample"); EXPECT_GT(r.hits[0].similarity, 0.99f);
    EXPECT_EQ(r.hits[0].evidence.session_id, "s"); EXPECT_EQ(r.hits[0].evidence.actual_offset_ms, 520); EXPECT_EQ(r.hits[0].evidence.uncertainty_ms, 252);
}

TEST(PopHistory, RejectsTruncatedJpegsDuplicatesAndEnforcesTheBounds) {
    Fx f; f.schedule("s", "ad-a"); std::string e; ASSERT_TRUE(f.store.history_open(e)) << e;
    HistoryLimits lim; lim.max_frames_per_session = 3; lim.max_pending_embeddings = 100; HistoryFrame o;
    auto bad = hist_in("s", 0, 0); bad.jpeg.resize(bad.jpeg.size() / 2);
    EXPECT_FALSE(f.store.add_history_frame(bad, lim, o, e)); EXPECT_NE(e.find("complete JPEG"), std::string::npos) << e;
    auto junk = hist_in("s", 0, 0); junk.jpeg.assign(300, 7); EXPECT_FALSE(f.store.add_history_frame(junk, lim, o, e));
    EXPECT_FALSE(f.store.add_history_frame(hist_in("nosuch", 0, 0), lim, o, e)); EXPECT_EQ(e, "unknown session");
    EXPECT_TRUE(f.store.add_history_frame(hist_in("s", 0, 0), lim, o, e)) << e;
    EXPECT_FALSE(f.store.add_history_frame(hist_in("s", 0, 0, 5), lim, o, e)); EXPECT_EQ(e, "duplicate");
    EXPECT_TRUE(f.store.add_history_frame(hist_in("s", 1, 0), lim, o, e)) << e;
    EXPECT_TRUE(f.store.add_history_frame(hist_in("s", 2, 0), lim, o, e)) << e;
    EXPECT_FALSE(f.store.add_history_frame(hist_in("s", 3, 0), lim, o, e)); EXPECT_EQ(e, "session limit");
    f.schedule("t", "ad-a"); lim.max_frames_per_session = 100; lim.max_pending_embeddings = 3;
    EXPECT_FALSE(f.store.add_history_frame(hist_in("t", 0, 0), lim, o, e)) << "3 un-embedded rows already pending"; EXPECT_EQ(e, "embedding backlog");
    EXPECT_EQ(f.store.history_stats().rows, 3u);
    int files = 0; for (auto& p : fs::recursive_directory_iterator(fs::path(f.dir.str()) / "pop")) if (p.path().extension() == ".tmp") ++files;
    EXPECT_EQ(files, 0) << "no temporary files left behind";
}

TEST(PopHistory, ACheckpointFrameIsReusedNotStoredOrCountedTwice) {
    Fx f; f.media("ad-a", 0); f.schedule("c", "ad-a"); f.full_play("c");
    std::string e; ASSERT_TRUE(f.store.history_open(e)) << e;
    shot_via(f, "c", 1, 0, 3000, "resilience_grpc", 0);                          // checkpoint at utc kT0+1000+3000
    HistoryLimits lim; HistoryFrame same, other;
    ASSERT_TRUE(f.store.add_history_frame(hist_in("c", 3, 0, 0, kT0 + 1000 + 3000), lim, same, e)) << e;       // the very same instant
    ASSERT_TRUE(f.store.add_history_frame(hist_in("c", 4, 0, 6, kT0 + 1000 + 4000), lim, other, e)) << e;      // a different instant
    EXPECT_NE(same.evidence_id, 0); EXPECT_EQ(same.role, "history_and_checkpoint"); EXPECT_FALSE(same.image_available); EXPECT_TRUE(same.path.empty()) << "no second JPEG";
    EXPECT_EQ(other.evidence_id, 0); EXPECT_TRUE(other.image_available);
    EXPECT_EQ(f.store.history_stats().linked_to_checkpoint, 1u);
    int n; while ((n = f.store.embed_pending_history(f.prov, 10, e)) > 0) {}
    std::vector<uint8_t> j; EXPECT_TRUE(f.store.history_jpeg(same.frame_id, j)) << "served from the checkpoint's file"; EXPECT_FALSE(j.empty());
    EvidenceSearchOptions so; so.has_min_similarity = false; so.top_k = 10; EvidenceSearchResult r;
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0), so, r, e)) << e;
    EXPECT_EQ(r.scanned, 2u) << "checkpoint + one extra history frame; the reused one is not scanned again";
    EXPECT_EQ(r.coverage.checkpoint_rows, 1u); EXPECT_EQ(r.coverage.history_rows, 1u); EXPECT_EQ(r.coverage.rows_searchable, 2u);
    // a checkpoint stored AFTER the history frame of the same instant also wins at search time
    Fx g; g.media("ad-a", 0); g.schedule("d", "ad-a"); g.full_play("d"); ASSERT_TRUE(g.store.history_open(e)) << e;
    ASSERT_TRUE(g.store.add_history_frame(hist_in("d", 3, 0, 0, kT0 + 1000 + 3000), lim, same, e)) << e;
    EXPECT_EQ(same.evidence_id, 0); while (g.store.embed_pending_history(g.prov, 10, e) > 0) {}
    shot_via(g, "d", 1, 0, 3000, "resilience_grpc", 0);
    ASSERT_TRUE(g.store.search_evidence(g.prov, make_jpeg(0), so, r, e)) << e;
    EXPECT_EQ(r.scanned, 1u) << "only the checkpoint is scanned";
}

TEST(PopHistory, HistoryNeverChangesAVerificationVerdict) {
    Fx f; f.media("ad-a", 0); f.media("ad-b", 1); f.schedule("v", "ad-a"); f.full_play("v");
    for (int i = 0; i < 3; ++i) f.shot("v", i + 1, 0, 3000 + i * 12000, i * 3);
    const Verification before = f.verify("v");
    ASSERT_EQ(before.status, PopStatus::PlaybackCorroborated) << before.reason;
    std::string e; ASSERT_TRUE(f.store.history_open(e)) << e;
    HistoryLimits lim; HistoryFrame o;
    for (int k = 0; k < 30; ++k) ASSERT_TRUE(f.store.add_history_frame(hist_in("v", k, k % 2 ? 1 : 2, k), lim, o, e)) << e;   // wildly different content: ad B and kind 2
    while (f.store.embed_pending_history(f.prov, 50, e) > 0) {}
    const Verification after = f.verify("v");
    EXPECT_EQ(after.status, before.status); EXPECT_EQ(after.reason, before.reason);
    EXPECT_EQ(after.frames_total, before.frames_total); EXPECT_EQ(after.frames_ok, before.frames_ok); EXPECT_EQ(after.frames_matched, before.frames_matched);
    EXPECT_DOUBLE_EQ(after.mean_sim_expected, before.mean_sim_expected);
    EXPECT_EQ(f.store.list_evidence("v").size(), 3u) << "the checkpoints are still exactly three";
}

TEST(PopHistory, SearchFindsAnyMomentAndFiltersBySessionTimeAdvertisementAndRole) {
    Fx f; history_fixture(f); std::string e; EvidenceSearchResult r; EvidenceSearchOptions o; o.top_k = 50; o.has_min_similarity = false;
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 4 * 11), o, r, e)) << e;          // ad A as it looks at second 11
    ASSERT_FALSE(r.hits.empty());
    EXPECT_EQ(r.scanned, 50u);
    EXPECT_EQ(r.hits[0].evidence.role, "history_sample"); EXPECT_EQ(r.hits[0].evidence.sec_idx, 11);
    EXPECT_GT(r.hits[0].similarity, 0.99f) << "an exact moment is found even though no verification checkpoint exists there";
    EXPECT_EQ(r.coverage.history_rows, 50u); EXPECT_EQ(r.coverage.checkpoint_rows, 0u); EXPECT_EQ(r.coverage.sessions, 3u);
    o.session_id = "h2"; ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0, 4 * 11), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 20u); for (auto& h : r.hits) EXPECT_EQ(h.evidence.session_id, "h2");
    o.session_id = ""; o.start_utc_ms = kT0 + 600000; o.end_utc_ms = kT0 + 700000; ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 20u) << "time window selects the second session only";
    o.start_utc_ms = o.end_utc_ms = 0; o.media_id = "ad-b"; ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(1), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 10u);
    o.media_id = ""; o.role = "verification_checkpoint"; ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 0u); o.role = "history_sample"; ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0), o, r, e)) << e; EXPECT_EQ(r.scanned, 50u);
    o.role = ""; o.capture_method = "brightscript_screenshot"; ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 0u) << "history is Weston/VOP2 only and never appears in screenshot results";
}

TEST(PopHistory, FinalizeDropsFramesAfterTheEndAndRecordsThePlayedMedia) {
    Fx f; history_fixture(f);
    // h1 ended at second 10: samples 10..19 (utc >= end - 400 ms) must go
    const int64_t end = kT0 + 10 * 1000;
    EXPECT_EQ(f.store.history_finalize_session("h1", end, 400, "ad-b"), 10u);
    auto rows = f.store.list_history("h1", 100, 0);
    ASSERT_EQ(rows.size(), 10u); for (auto& r : rows) { EXPECT_LE(r.utc_ms, end - 400); EXPECT_EQ(r.played_media_id, "ad-b"); }
    int jpgs = 0; for (auto& p : fs::recursive_directory_iterator(fs::path(f.dir.str()) / "pop" / "history")) if (p.path().extension() == ".jpg") ++jpgs;
    EXPECT_EQ(jpgs, 10 + 20 + 10) << "the removed frames' files are gone too";
}

TEST(PopHistory, QuotaAndRetentionRemoveImagesFirstAndKeepTheSearchableVectors) {
    Fx f; history_fixture(f);
    const HistoryStats s0 = f.store.history_stats(); ASSERT_EQ(s0.rows, 50u); ASSERT_EQ(s0.images_retained, 50u);
    const uint64_t quota = s0.image_bytes / 2;
    HistoryPruneResult p = f.store.prune_history(0, quota, 0);
    const HistoryStats s1 = f.store.history_stats();
    EXPECT_GT(p.images_removed, 0u); EXPECT_LE(s1.image_bytes, quota); EXPECT_EQ(s1.rows, 50u) << "rows and vectors stay";
    EvidenceSearchResult r; EvidenceSearchOptions o; o.has_min_similarity = false; o.top_k = 50; std::string e;
    ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0), o, r, e)) << e;
    EXPECT_EQ(r.scanned, 50u); EXPECT_LT(r.coverage.images_retained, 50u);
    bool saw_expired = false; for (auto& h : r.hits) if (!h.evidence.image_available) saw_expired = true;
    EXPECT_TRUE(saw_expired) << "expired JPEGs are still hits, flagged as not retained";
    o.require_image = true; ASSERT_TRUE(f.store.search_evidence(f.prov, make_jpeg(0), o, r, e)) << e; EXPECT_EQ(r.scanned, s1.images_retained);
    // oldest first: the first session lost its images before the last one
    auto h1 = f.store.list_history("h1", 100, 0); EXPECT_FALSE(h1.front().image_available);
    // metadata retention deletes the rows themselves
    p = f.store.prune_history(0, 0, kT0 + 600000);
    EXPECT_EQ(p.rows_removed, 20u); EXPECT_EQ(f.store.history_stats().rows, 30u);
    p = f.store.prune_history(kT0 + 2'000'000, 0, 0);
    EXPECT_EQ(f.store.history_stats().images_retained, 0u);
}

TEST(PopHistory, TheRowBoundKeepsTheExactScanSmallByDroppingTheOldestRows) {
    Fx f; history_fixture(f);
    const HistoryPruneResult p = f.store.prune_history(0, 0, 0, 35);
    EXPECT_EQ(p.rows_removed, 15u); EXPECT_EQ(f.store.history_stats().rows, 35u);
    const auto left = f.store.list_history("h1", 100, 0);
    ASSERT_EQ(left.size(), 5u) << "the 15 oldest rows all came from the first session";
    EXPECT_EQ(left.front().sec_idx, 15) << "the oldest samples went first";
    int jpgs = 0; for (auto& q : fs::recursive_directory_iterator(fs::path(f.dir.str()) / "pop" / "history")) if (q.path().extension() == ".jpg") ++jpgs;
    EXPECT_EQ(jpgs, 35) << "and so did their files";
}

// ---- scaling benchmark (not part of the normal run): ./visualmemory_tests --gtest_also_run_disabled_tests --gtest_filter='PopHistoryBench.*' ----------
#include <chrono>
#include <random>
TEST(PopHistoryBench, DISABLED_ExactSearchLatencyVersusHistorySize) {
    constexpr int kDim = 1280;
    vmtest::TempDir dir; MockEmbeddingProvider prov(kDim);
    PopConfig c; c.root_directory = dir.str(); c.sync_full = false;
    std::string e; int64_t model = 0;
    {
        PopStore s(c); ASSERT_TRUE(s.open(e)) << e; ASSERT_TRUE(s.history_open(e)) << e;
        PopSession ps; ps.session_id = "bench"; ps.expected_media_id = "ad-a"; ASSERT_TRUE(s.schedule_session(ps, e)) << e;
        HistoryFrame o; HistoryLimits lim; ASSERT_TRUE(s.add_history_frame(hist_in("bench", 0, 0), lim, o, e)) << e;
        ASSERT_EQ(s.embed_pending_history(prov, 4, e), 1) << e;                          // registers the model row
    }
    const std::string path = (fs::path(dir.str()) / "pop.db").string();
    std::mt19937 rng(7); std::normal_distribution<float> nd(0.f, 1.f);
    auto q = make_jpeg(0, 0);
    int64_t have = 1;
    for (int64_t target : {30, 300, 3000, 30000, 100000}) {
        {   // top the table up with random unit vectors straight into SQLite (a real store would hold JPEG-less rows for these as well)
            sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
            sqlite3_exec(db, "PRAGMA synchronous=OFF; BEGIN;", nullptr, nullptr, nullptr);
            sqlite3_stmt* mid = nullptr; sqlite3_prepare_v2(db, "SELECT model_id FROM pop_models LIMIT 1;", -1, &mid, nullptr); ASSERT_EQ(sqlite3_step(mid), SQLITE_ROW); model = sqlite3_column_int64(mid, 0); sqlite3_finalize(mid);
            sqlite3_stmt* ins = nullptr;
            ASSERT_EQ(sqlite3_prepare_v2(db, "INSERT INTO display_history_frames(session_id,sec_idx,source,utc_ms,model_id,embedding,embedded,created_at) VALUES('bench',?,'main_display',?,?,?,1,0);", -1, &ins, nullptr), SQLITE_OK);
            std::vector<float> v(kDim);
            for (; have < target; ++have) {
                double n2 = 0; for (auto& x : v) { x = nd(rng); n2 += x * x; } const float inv = static_cast<float>(1.0 / std::sqrt(n2)); for (auto& x : v) x *= inv;
                sqlite3_bind_int64(ins, 1, have); sqlite3_bind_int64(ins, 2, kT0 + have * 1000); sqlite3_bind_int64(ins, 3, model);
                sqlite3_bind_blob(ins, 4, v.data(), kDim * 4, SQLITE_TRANSIENT); sqlite3_step(ins); sqlite3_reset(ins);
            }
            sqlite3_finalize(ins); sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr); sqlite3_close(db);
        }
        PopStore s(c); ASSERT_TRUE(s.open(e)) << e;
        EvidenceSearchOptions o; o.top_k = 10; o.has_min_similarity = false;
        double best_scan = 1e9, sum = 0; int reps = 5; uint64_t scanned = 0;
        for (int i = 0; i < reps; ++i) { EvidenceSearchResult r; ASSERT_TRUE(s.search_evidence(prov, q, o, r, e)) << e; best_scan = std::min(best_scan, r.scan_ms); sum += r.scan_ms; scanned = r.scanned; }
        std::printf("BENCH rows=%-7lld scanned=%-7llu scan_ms best=%8.2f mean=%8.2f  db=%.1f MB\n", static_cast<long long>(target), static_cast<unsigned long long>(scanned), best_scan, sum / reps,
                    static_cast<double>(fs::file_size(path)) / 1048576.0);
    }
}

// ---- Time-based display history lookup (read-only; stored timestamps only) ---------------------------------------------------------------------------
TEST(PopTimeLookup, NearestIsDeterministicTiesResolveToTheEarlierFrameAndTheToleranceIsHonoured) {
    Fx f; history_fixture(f); NearestDisplayFrame n;
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1500, 100, "", n));
    EXPECT_TRUE(n.found); EXPECT_EQ(n.diff_ms, 0); EXPECT_EQ(n.frame.session_id, "h1"); EXPECT_EQ(n.frame.index, 1);
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1000, 1000, "", n));            // 500 ms from sample 0 and from sample 1: the earlier one wins
    EXPECT_TRUE(n.found); EXPECT_EQ(n.frame.index, 0); EXPECT_EQ(n.diff_ms, -500);
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1100, 1000, "", n));
    EXPECT_EQ(n.frame.index, 1); EXPECT_EQ(n.diff_ms, 400);
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1100, 399, "", n));             // nearest is 400 ms away: outside a 399 ms tolerance
    EXPECT_FALSE(n.found); EXPECT_TRUE(n.has_nearest); EXPECT_EQ(n.diff_ms, 400);
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1100, 400, "", n)); EXPECT_TRUE(n.found) << "the tolerance is inclusive";
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 100000, 500, "", n));          // between sessions: a gap, not a match
    EXPECT_FALSE(n.found); EXPECT_EQ(n.frames_in_store, 50u); EXPECT_EQ(n.oldest_utc_ms, kT0 + 500); EXPECT_EQ(n.newest_utc_ms, kT0 + 1200000 + 500 + 9000);
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1100, 100000, "h3", n));       // session filter
    EXPECT_EQ(n.frame.session_id, "h3");
    for (int i = 0; i < 5; ++i) { NearestDisplayFrame m; ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1000, 1000, "", m)); EXPECT_EQ(m.frame.index, 0); EXPECT_EQ(m.frame.session_id, "h1"); }
}

TEST(PopTimeLookup, IdenticalInstantsResolveToTheLowestId) {
    Fx f; f.media("ad-a", 0); f.schedule("a", "ad-a"); f.schedule("b", "ad-a"); f.full_play("a"); f.full_play("b");
    std::string e; ASSERT_TRUE(f.store.history_open(e)) << e; HistoryLimits lim; HistoryFrame oa, ob;
    ASSERT_TRUE(f.store.add_history_frame(hist_in("b", 0, 0, 0, kT0 + 5000), lim, ob, e)) << e;   // "b" is stored first: lower id
    ASSERT_TRUE(f.store.add_history_frame(hist_in("a", 0, 0, 0, kT0 + 5000), lim, oa, e)) << e;
    ASSERT_LT(ob.frame_id, oa.frame_id);
    NearestDisplayFrame n; ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 5000, 10, "", n));
    EXPECT_EQ(n.frame.frame_id, ob.frame_id);
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 5000, 10, "a", n)); EXPECT_EQ(n.frame.session_id, "a");
}

TEST(PopTimeLookup, OnlyWestonDisplayFramesAreReturnedAndALinkedCheckpointIsReportedOnce) {
    Fx f; f.media("ad-a", 0); f.schedule("s", "ad-a"); f.full_play("s");
    std::string e; ASSERT_TRUE(f.store.history_open(e)) << e;
    shot_via(f, "s", 1, 0, 3000, "brightscript_screenshot", 1);                       // a BrightScript screenshot is not Weston/VOP2 output
    shot_via(f, "s", 2, 0, 15000, "resilience_grpc", 2, 275);                          // an unlinked Weston checkpoint
    shot_via(f, "s", 3, 0, 27000, "resilience_grpc", 3, 275);                          // a Weston checkpoint that history links
    HistoryLimits lim; HistoryFrame o;
    HistoryFrameIn link = hist_in("s", 27, 0); link.utc_ms = kT0 + 1000 + 27000; link.actual_offset_ms = 27007; link.capture_method = "resilience_grpc";
    ASSERT_TRUE(f.store.add_history_frame(link, lim, o, e)) << e;
    NearestDisplayFrame n;
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1000 + 3000, 5000, "", n));
    EXPECT_FALSE(n.found) << "the only frame at +3 s is a screenshot-method checkpoint: excluded, the next Weston frame is 12 s away";
    ASSERT_TRUE(n.has_nearest); EXPECT_NE(n.frame.capture_method, "brightscript_screenshot"); EXPECT_NE(n.frame.utc_ms, kT0 + 1000 + 3000);
    EXPECT_EQ(n.frames_in_store, 2u) << "an unlinked checkpoint and one history row (which carries the linked checkpoint)";
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1000 + 15000, 10, "", n));
    EXPECT_TRUE(n.found); EXPECT_EQ(n.frame.kind, "verification_checkpoint"); EXPECT_EQ(n.frame.frame_id, 0); EXPECT_GT(n.frame.evidence_id, 0); EXPECT_EQ(n.frame.uncertainty_ms, 275);
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1000 + 27000, 10, "", n));
    EXPECT_TRUE(n.found); EXPECT_NE(n.frame.kind, "verification_checkpoint") << "history row, with the checkpoint attached";
    const auto rows = f.store.display_frames_between(kT0, kT0 + 60000, "s", 100);
    int at27 = 0; for (const auto& r : rows) if (r.utc_ms == kT0 + 1000 + 27000) ++at27;
    EXPECT_EQ(at27, 1) << "one entry, not a history row plus its checkpoint";
    EXPECT_EQ(rows.size(), 2u);
}

TEST(PopTimeLookup, AnExpiredJpegKeepsItsMetadataAndSaysSo) {
    Fx f; history_fixture(f);
    HistoryPruneResult r = f.store.prune_history(kT0 + 5000, 1ull << 40, 0, 0);       // JPEG retention: remove images older than +5 s, keep the rows
    EXPECT_GT(r.images_removed, 0u);
    NearestDisplayFrame n; ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 2500, 100, "", n));
    EXPECT_TRUE(n.found); EXPECT_FALSE(n.frame.image_available) << "JPEG gone"; EXPECT_TRUE(n.frame.embedded) << "vector and row are kept";
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 12500, 100, "", n)); EXPECT_TRUE(n.frame.image_available);
    { std::vector<uint8_t> b; EXPECT_TRUE(f.store.history_jpeg(n.frame.frame_id, b)); EXPECT_FALSE(b.empty()); }
    r = f.store.prune_history(0, 1ull << 40, kT0 + 5000, 0);                           // metadata retention: remove rows older than +5 s
    EXPECT_GT(r.rows_removed, 0u);
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 2500, 100, "", n)); EXPECT_FALSE(n.found) << "no row left: a no-evidence answer";
}

TEST(PopTimeLookup, TheWindowQueryOrdersFramesFindsSessionsAndReportsTruncation) {
    Fx f; history_fixture(f);
    bool tr = true;
    auto rows = f.store.display_frames_between(kT0 + 4500, kT0 + 8500, "", 100, &tr);
    ASSERT_EQ(rows.size(), 5u); EXPECT_FALSE(tr);
    for (size_t i = 1; i < rows.size(); ++i) EXPECT_LT(rows[i - 1].utc_ms, rows[i].utc_ms);
    rows = f.store.display_frames_between(kT0, kT0 + 2000000, "", 7, &tr); EXPECT_EQ(rows.size(), 7u); EXPECT_TRUE(tr);
    const auto sess = f.store.list_sessions_overlapping(kT0 + 5000, kT0 + 6000, 10);          // the fixture's events put all three sessions at +0.3 s..+30.3 s
    EXPECT_EQ(sess.size(), 3u);
    rows = f.store.display_frames_between(kT0 + 5000, kT0 + 6000, "", 100);
    ASSERT_EQ(rows.size(), 1u); EXPECT_EQ(rows[0].session_id, "h1") << "only h1 has a frame in that second";
    EXPECT_TRUE(f.store.list_sessions_overlapping(kT0 + 31000, kT0 + 60000, 10).empty()) << "between sessions";
}

TEST(PopTimeLookup, SimilarityUsesStoredVectorsAndReportsWhenAVectorIsMissing) {
    Fx f; history_fixture(f);
    NearestDisplayFrame n; ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1500, 10, "h1", n));
    auto s = f.store.expected_similarity("h1", {FrameSimQuery{n.frame.frame_id, n.frame.evidence_id}});
    ASSERT_EQ(s.size(), 1u); EXPECT_TRUE(s[0].ok) << s[0].reason; EXPECT_GT(s[0].sim, 0.8f); EXPECT_EQ(s[0].refs, 4);
    ASSERT_TRUE(f.store.nearest_display_frame(kT0 + 1200000 + 1500, 10, "h3", n));            // ad-B frames against ad-B references
    s = f.store.expected_similarity("h3", {FrameSimQuery{n.frame.frame_id, n.frame.evidence_id}}); EXPECT_TRUE(s[0].ok); EXPECT_GT(s[0].sim, 0.8f);
    s = f.store.expected_similarity("h1", {FrameSimQuery{n.frame.frame_id, n.frame.evidence_id}});   // the same ad-B frame against ad-A references
    EXPECT_TRUE(s[0].ok); EXPECT_LT(s[0].sim, 0.8f) << "a different advertisement is clearly lower";
    f.schedule("p", "ad-a"); f.full_play("p"); HistoryLimits lim; HistoryFrame o; std::string e;
    ASSERT_TRUE(f.store.add_history_frame(hist_in("p", 0, 0, 0, kT0 + 2400000), lim, o, e)) << e;   // not embedded yet
    s = f.store.expected_similarity("p", {FrameSimQuery{o.frame_id, 0}}); EXPECT_FALSE(s[0].ok); EXPECT_NE(s[0].reason.find("vector"), std::string::npos) << s[0].reason;
    s = f.store.expected_similarity("nosuch", {FrameSimQuery{o.frame_id, 0}}); EXPECT_FALSE(s[0].ok);
}

TEST(PopTimeLookup, TheQueriesAreReadOnlyAndUseTheTimestampIndexes) {
    Fx f; history_fixture(f);
    const std::string db = (fs::path(f.dir.str()) / "pop.db").string();
    std::ifstream a(db, std::ios::binary); std::string before((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>()); a.close();
    NearestDisplayFrame n; for (int i = 0; i < 20; ++i) ASSERT_TRUE(f.store.nearest_display_frame(kT0 + i * 777, 1000, "", n));
    f.store.display_frames_between(kT0, kT0 + 100000, "", 100); f.store.list_sessions_overlapping(kT0, kT0 + 100000, 10);
    sqlite3* h = nullptr; ASSERT_EQ(sqlite3_open_v2(db.c_str(), &h, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    auto plan = [&](const char* sql) {
        std::string out; sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(h, (std::string("EXPLAIN QUERY PLAN ") + sql).c_str(), -1, &st, nullptr) == SQLITE_OK)
            while (sqlite3_step(st) == SQLITE_ROW) out += reinterpret_cast<const char*>(sqlite3_column_text(st, 3)) + std::string(";");
        sqlite3_finalize(st); return out;
    };
    EXPECT_NE(plan("SELECT utc_ms FROM display_history_frames h WHERE h.status='ok' AND h.utc_ms<=5 ORDER BY h.utc_ms DESC LIMIT 1").find("ix_pop_hist_time"), std::string::npos);
    EXPECT_NE(plan("SELECT utc_ms FROM display_capture_evidence e WHERE e.status='ok' AND e.capture_method='resilience_grpc' AND e.utc_ms>5 ORDER BY e.utc_ms ASC LIMIT 1").find("ix_pop_evd_time"), std::string::npos);
    EXPECT_NE(plan("SELECT 1 FROM playback_sessions s WHERE s.start_utc_ms>0 AND s.start_utc_ms<=5").find("ix_pop_sess_start"), std::string::npos);
    sqlite3_close(h);
    EXPECT_EQ(user_version_of(db), 2);
    std::ifstream c(db, std::ios::binary); std::string after((std::istreambuf_iterator<char>(c)), std::istreambuf_iterator<char>());
    EXPECT_EQ(before, after) << "lookups never write to pop.db";
}
