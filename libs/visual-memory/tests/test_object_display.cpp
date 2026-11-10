// SPDX-License-Identifier: MIT
// D4 objects from the BrightSign display (Weston/VOP2 frames kept by Proof of Play): frame sources, schema v3 (source-aware identity),
// provenance, isolation from camera objects (ids from different databases must never collide or join), indexing through the shared worker.
#include "visualmemory/mock_embedding_provider.h"
#include "visualmemory/object_detector.h"
#include "visualmemory/object_frame_source.h"
#include "visualmemory/object_index.h"
#include "visualmemory/object_indexer.h"
#include "visualmemory/pop.h"
#include "visualmemory/reader.h"
#include "visualmemory/visual_memory.h"
#include "jpeg_decoder.h"
#include "test_util.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>

using namespace visualmemory;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kT0 = 1'767'261'600'000LL;
struct Rgb { uint8_t r, g, b; };
const Rgb kRed{220, 30, 30}, kGreen{30, 200, 40}, kBlue{30, 40, 220}, kYellow{230, 220, 30};

// A 640x360 Weston/VOP2-style frame: 4:3 content letterboxed with black side bars (x < 80 and x >= 560), one coloured square in the picture.
std::vector<uint8_t> display_jpeg(Rgb obj, int sx, int sy, int sw = 60, int sh = 60, uint8_t bg = 128) {
    const int W = 640, H = 360;
    std::vector<uint8_t> rgb(static_cast<size_t>(W) * H * 3, 0);
    for (int y = 0; y < H; ++y) for (int x = 80; x < 560; ++x) { uint8_t* p = &rgb[(static_cast<size_t>(y) * W + x) * 3]; p[0] = p[1] = p[2] = bg; }
    for (int y = sy; y < sy + sh; ++y) for (int x = sx; x < sx + sw; ++x) { uint8_t* p = &rgb[(static_cast<size_t>(y) * W + x) * 3]; p[0] = obj.r; p[1] = obj.g; p[2] = obj.b; }
    std::vector<uint8_t> jpg; std::string e;
    EXPECT_TRUE(detail::encode_rgb_jpeg(rgb.data(), W, H, 92, jpg, e)) << e;
    return jpg;
}

std::vector<DetectedObject> colour_detector(const ImageView& v) {                  // same mock as test_object_auto.cpp
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; Rgb c{0, 0, 0};
    for (int y = 0; y < v.height; ++y) for (int x = 0; x < v.width; ++x) {
        const uint8_t* p = v.data + (static_cast<size_t>(y) * v.width + x) * 3;
        const int mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
        if (mx - mn > 100) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); c = {p[0], p[1], p[2]}; }
    }
    std::vector<DetectedObject> out;
    if (x1 < 0) return out;
    DetectedObject d; d.x = x0; d.y = y0; d.w = x1 - x0 + 1; d.h = y1 - y0 + 1;
    if (c.r > 150 && c.g > 150) { d.class_id = 63; d.confidence = 0.28f; }
    else if (c.r > 150 && c.g < 90) { d.class_id = 39; d.confidence = 0.90f; }
    else if (c.g > 150) { d.class_id = 41; d.confidence = 0.60f; }
    else { d.class_id = 0; d.confidence = 0.80f; }
    d.class_name = coco80_name(d.class_id);
    out.push_back(d);
    return out;
}

ObjectIndexerConfig icfg() {
    ObjectIndexerConfig c; c.class_ids = {39, 40, 41, 63, 67, 73}; c.min_confidence = 0.30f; c.poll_ms = 50; c.backfill_pause_ms = 0; c.backoff_ms = 100;
    return c;
}

Config cam_cfg(const std::string& root) {
    Config c; c.root_directory = root; c.stored_width = 160; c.stored_height = 120; c.jpeg_quality = 95;
    c.max_pending_frames = 16; c.retention_hours = 0; c.max_storage_bytes = 0; c.embedding.enabled = false;
    return c;
}

// camera + Proof-of-Play + objects, all in temp dirs
struct Rig {
    vmtest::TempDir cam, pop, obj;
    std::unique_ptr<PopStore> store;
    std::unique_ptr<VisualMemoryReader> rd;
    std::unique_ptr<ObjectIndex> oi;
    std::unique_ptr<CameraObjectSource> cams;
    std::unique_ptr<DisplayObjectSource> disp;
    std::shared_ptr<MockEmbeddingProvider> prov = std::make_shared<MockEmbeddingProvider>(32);
    std::shared_ptr<MockEmbeddingProvider> pop_prov = std::make_shared<MockEmbeddingProvider>(64);
    std::shared_ptr<MockObjectDetector> det = std::make_shared<MockObjectDetector>(colour_detector);
    std::unique_ptr<ObjectIndexer> ix;
    std::vector<int64_t> hist_ids, ckpt_ids;           // display ids (history frame ids / checkpoint evidence ids)
    std::vector<int64_t> cam_ids;

    void ev(const std::string& sid, const std::string& type, int64_t t, const std::string& reason = "", int64_t actual = -1, const std::string& played = "") {
        PopEventIn x; x.session_id = sid; x.type = type; x.utc_ms = kT0 + t; x.mono_ms = t; x.reason = reason; x.actual_play_ms = actual; x.played_media_id = played;
        std::string e; ASSERT_TRUE(store->apply_event(x, e)) << e;
    }
    void session(const std::string& sid, const std::string& expected, const std::string& played, int64_t t0 = 0) {
        PopSession s; s.session_id = sid; s.expected_media_id = expected; s.slot_ms = 30000; s.captures_requested = 3;
        std::string e; ASSERT_TRUE(store->schedule_session(s, e)) << e;
        ev(sid, "session_scheduled_run", t0, "", -1, played); ev(sid, "play_requested", t0 + 10); ev(sid, "play_started", t0 + 300, "", -1, played);
        ev(sid, "play_stopped", t0 + 30300, "slot_end", -1, played); ev(sid, "session_end", t0 + 30310, "slot_end", 30100, played);
    }
    int64_t history(const std::string& sid, int k, const std::vector<uint8_t>& jpg, int64_t utc, const std::string& method = "resilience_grpc", const std::string& expected = "ad-a") {
        HistoryFrameIn h; h.session_id = sid; h.sec_idx = k; h.target_offset_ms = 500 + k * 1000; h.actual_offset_ms = 520 + k * 1000; h.utc_ms = utc; h.mono_ms = 1000 + k * 1000;
        h.uncertainty_ms = 252; h.expected_media_id = expected; h.capture_method = method; h.provenance = "weston_vop2_writeback p7"; h.jpeg = jpg;
        HistoryFrame o; std::string e; HistoryLimits lim;
        if (!store->add_history_frame(h, lim, o, e)) { ADD_FAILURE() << e; return 0; }
        hist_ids.push_back(o.frame_id); return o.frame_id;
    }
    int64_t checkpoint(const std::string& sid, int idx, const std::vector<uint8_t>& jpg, int64_t utc, const std::string& method = "resilience_grpc") {
        EvidenceIn in; in.session_id = sid; in.idx = idx; in.target_offset_ms = idx * 5000; in.actual_offset_ms = idx * 5000 + 7; in.utc_ms = utc; in.capture_method = method;
        in.provenance = "weston_vop2_writeback p7"; in.uncertainty_ms = 275; in.jpeg = jpg;
        Evidence o; std::string e;
        if (!store->add_evidence(*pop_prov, in, o, e)) { ADD_FAILURE() << e; return 0; }
        ckpt_ids.push_back(o.evidence_id); return o.evidence_id;
    }
    void camera_frames(int n, int64_t base) {
        VisualMemory vm(cam_cfg(cam.str())); ASSERT_TRUE(vm.start());
        for (int i = 0; i < n; ++i) {
            OwnedFrame f; f.width = 160; f.height = 120; f.pixel_format = PixelFormat::BGR24; f.stride0 = 160 * 3; f.pixels.assign(160 * 120 * 3, 128);
            for (int y = 20; y < 60; ++y) for (int x = 20 + i * 10; x < 60 + i * 10 && x < 160; ++x) { uint8_t* p = &f.pixels[(static_cast<size_t>(y) * 160 + x) * 3]; p[0] = 30; p[1] = 30; p[2] = 220; }   // BGR: red square
            f.metadata.source_id = "cam"; f.metadata.source_type = "file"; f.metadata.device_id = "t"; f.metadata.capture_utc_ms = base + i * 1000; f.metadata.source_sequence = i + 1;
            bool ok = false; for (int t = 0; t < 500 && !ok; ++t) { OwnedFrame c = f; ok = vm.try_submit(std::move(c)); if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            ASSERT_TRUE(ok);
        }
        ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == static_cast<uint64_t>(n); })); vm.stop();
        VisualMemoryReader r({cam.str()}); std::string e; ASSERT_TRUE(r.open(e)) << e;
        for (const auto& fr : r.list_frames({}, 100, 0, false)) cam_ids.push_back(fr.frame.id);
    }
    void open() {
        PopConfig pc; pc.root_directory = pop.str(); store = std::make_unique<PopStore>(pc);
        std::string e; ASSERT_TRUE(store->open(e)) << e; ASSERT_TRUE(store->history_open(e)) << e;
    }
    void open_objects() {
        std::string e;
        if (!cam_ids.empty()) {                                  // a camera store exists only when the test created frames
            rd = std::make_unique<VisualMemoryReader>(ReaderConfig{cam.str()}); ASSERT_TRUE(rd->open(e)) << e;
            cams = std::make_unique<CameraObjectSource>(*rd);
        }
        ObjectIndexConfig oc; oc.root_directory = obj.str(); oi = std::make_unique<ObjectIndex>(oc); ASSERT_TRUE(oi->open(e)) << e;
        disp = std::make_unique<DisplayObjectSource>(*store);
    }
    ObjectSources srcs() { return ObjectSources{cams.get(), disp.get()}; }
    void make_indexer(ObjectIndexerConfig c, bool live_cam, bool live_disp, int fpm = 600) {
        std::vector<IndexerSource> v;
        if (cams) v.push_back(IndexerSource{cams.get(), live_cam, fpm, 8});
        v.push_back(IndexerSource{disp.get(), live_disp, fpm, 8});
        ix = std::make_unique<ObjectIndexer>(c, *oi, v,
            [this](std::string&) { return std::shared_ptr<EmbeddingProvider>(prov); }, [this](std::string&) { return std::shared_ptr<ObjectDetector>(det); });
        ix->start();
    }
    bool wait_backfill(int ms = 15000) { return vmtest::wait_until([&] { const auto s = ix->stats().backfill_state; return s == "done" || s == "cancelled" || s == "error"; }, ms); }
};
} // namespace

// ------------------------------------------------------------------------------------------------ the display frame source
TEST(DisplaySource, FramesCarryTheirProofOfPlayFactsAndOnlyWestonFramesAreServed) {
    Rig r; r.open(); r.session("s1", "ad-a", "ad-a");
    const int64_t h1 = r.history("s1", 0, display_jpeg(kRed, 200, 100), kT0 + 500);
    const int64_t c1 = r.checkpoint("s1", 1, display_jpeg(kGreen, 300, 120), kT0 + 6000);
    const int64_t c_bs = r.checkpoint("s1", 2, display_jpeg(kRed, 100, 100), kT0 + 12000, "brightscript_screenshot");
    DisplayObjectSource d(*r.store); std::string e; ASSERT_TRUE(d.ensure_open(e)) << e;
    ObjSrcFrame f;
    ASSERT_TRUE(d.get("history", h1, f));
    EXPECT_EQ(f.source_type, "display"); EXPECT_EQ(f.kind, "history"); EXPECT_EQ(f.id, h1); EXPECT_EQ(f.session_id, "s1");
    EXPECT_EQ(f.expected_media_id, "ad-a"); EXPECT_EQ(f.reported_media_id, "ad-a");
    EXPECT_EQ(f.capture_utc_ms, kT0 + 500); EXPECT_EQ(f.playback_offset_ms, 520); EXPECT_EQ(f.time_uncertainty_ms, 252);
    EXPECT_EQ(f.capture_method, "resilience_grpc"); EXPECT_NE(f.provenance.find("weston_vop2"), std::string::npos);
    EXPECT_EQ(f.width, 640); EXPECT_EQ(f.height, 360); EXPECT_TRUE(f.image_available);
    ASSERT_TRUE(d.get("checkpoint", c1, f)); EXPECT_EQ(f.kind, "checkpoint"); EXPECT_EQ(f.time_uncertainty_ms, 275); EXPECT_EQ(f.session_id, "s1");
    EXPECT_FALSE(d.get("checkpoint", c_bs, f)) << "a BrightScript screenshot is not Weston/VOP2 output";
    EXPECT_FALSE(d.get("history", 9999, f)); EXPECT_FALSE(d.get("bogus", h1, f));
    std::vector<uint8_t> jpg;
    EXPECT_EQ(d.read_jpeg("history", h1, jpg, e), 0); EXPECT_EQ(d.read_jpeg("checkpoint", c1, jpg, e), 0);
    EXPECT_EQ(d.read_jpeg("history", 9999, jpg, e), 1);
    const auto all = d.list_range(0, 0, 100, true);
    EXPECT_EQ(all.size(), 2u);                                                   // the screenshot checkpoint is not a display frame
    EXPECT_GT(all[0].capture_utc_ms, all[1].capture_utc_ms);                     // newest first
    uint64_t stored = 0, with_image = 0; d.counts(stored, with_image); EXPECT_EQ(stored, 2u); EXPECT_EQ(with_image, 2u);
    EXPECT_EQ(d.list_after(kT0 + 1000, 10, nullptr).size(), 1u);
}

TEST(DisplaySource, AnExpiredJpegIsReportedAsExpiredWithItsMetadata) {
    Rig r; r.open(); r.session("s1", "ad-a", "ad-a");
    const int64_t h1 = r.history("s1", 0, display_jpeg(kRed, 200, 100), kT0 + 500);
    r.history("s1", 5, display_jpeg(kRed, 210, 100), kT0 + 5500);
    r.store->prune_history(kT0 + 2000, 1ull << 40, 0, 0);                        // JPEG retention: images older than +2 s go, rows stay
    DisplayObjectSource d(*r.store); std::vector<uint8_t> jpg; std::string e; ObjSrcFrame f;
    EXPECT_EQ(d.read_jpeg("history", h1, jpg, e), 2); EXPECT_NE(e.find("retention"), std::string::npos);
    ASSERT_TRUE(d.get("history", h1, f)); EXPECT_FALSE(f.image_available); EXPECT_EQ(f.session_id, "s1"); EXPECT_EQ(f.playback_offset_ms, 520);
}

// Found on the XT5: a history instant that coincides with a verification checkpoint stores NO second JPEG (the history row has evidence_id, no path, image_available=0);
// its picture is the checkpoint's. The display source must serve and index it, not report it expired.
TEST(DisplaySource, AHistoryInstantThatIsAlsoACheckpointIsServedFromTheCheckpointJpeg) {
    Rig r; r.open(); r.session("s1", "ad-a", "ad-a");
    const int64_t ck = r.checkpoint("s1", 1, display_jpeg(kRed, 200, 100), kT0 + 3000);
    const int64_t hl = r.history("s1", 2, display_jpeg(kRed, 200, 100), kT0 + 3000);          // same instant as the checkpoint: linked, no JPEG of its own
    HistoryFrame h; ASSERT_TRUE(r.store->get_history(hl, h));
    ASSERT_EQ(h.evidence_id, ck) << "the fixture must reproduce the player's linked row"; EXPECT_FALSE(h.image_available); EXPECT_TRUE(h.path.empty());
    DisplayObjectSource d(*r.store); ObjSrcFrame f; std::vector<uint8_t> jpg; std::string e;
    ASSERT_TRUE(d.get("history", hl, f));
    EXPECT_TRUE(f.image_available) << "the history row has no JPEG of its own but its checkpoint does"; EXPECT_TRUE(f.image_exists);
    EXPECT_EQ(d.read_jpeg("history", hl, jpg, e), 0) << e; EXPECT_GT(jpg.size(), 100u);
    EXPECT_EQ(f.session_id, "s1"); EXPECT_EQ(f.playback_offset_ms, 2520);
    const auto lst = d.list_range(0, 0, 50, false);
    int linked = 0; for (const auto& x : lst) if (x.kind == "history" && x.id == hl) { ++linked; EXPECT_TRUE(x.image_available); }
    EXPECT_EQ(linked, 1) << "listed once, as the history row";
}

TEST(DisplaySource, SharedCheckpointJpegSurvivesHistoryPruningAndALinkedFrameWhoseCheckpointImageIsGoneIsReportedUnavailable) {
    Rig r; r.open(); r.session("s1", "ad-a", "ad-a");
    const int64_t ck = r.checkpoint("s1", 1, display_jpeg(kRed, 200, 100), kT0 + 3000);
    const int64_t hl = r.history("s1", 2, display_jpeg(kRed, 200, 100), kT0 + 3000);          // linked: shares the checkpoint's JPEG
    const int64_t hp = r.history("s1", 5, display_jpeg(kGreen, 210, 100), kT0 + 5500);        // a plain history frame
    DisplayObjectSource d(*r.store); std::vector<uint8_t> jpg; std::string e; ObjSrcFrame f;
    r.store->prune_history(kT0 + 4000, 1ull << 40, 0, 0);                                   // history JPEG retention: everything older than +4 s goes
    EXPECT_EQ(d.read_jpeg("history", hl, jpg, e), 0) << "pruning history must not delete the checkpoint JPEG the linked row relies on (" << e << ")";
    EXPECT_EQ(d.read_jpeg("checkpoint", ck, jpg, e), 0) << "the D5 evidence is untouched";
    EXPECT_EQ(d.read_jpeg("history", hp, jpg, e), 0) << "a frame newer than the cut stays";
    r.store->prune_evidence(kT0 + 4000, 0);                                                  // evidence retention removes the checkpoint (and its file)
    ASSERT_TRUE(d.get("history", hl, f));
    EXPECT_FALSE(f.image_available) << "no checkpoint image left: the linked frame is unavailable, not a broken reference";
    EXPECT_EQ(d.read_jpeg("history", hl, jpg, e), 2); EXPECT_FALSE(e.empty());
    EXPECT_EQ(f.session_id, "s1") << "metadata stays";
}

// ------------------------------------------------------------------------------------------------ schema v3 migration (copied database)
namespace {
const char* kV2Ddl =
    "CREATE TABLE object_models(model_id INTEGER PRIMARY KEY,model_name TEXT NOT NULL,model_version TEXT NOT NULL,model_sha256 TEXT NOT NULL,preprocessing TEXT NOT NULL,dimension INTEGER NOT NULL,dtype TEXT NOT NULL,metric TEXT NOT NULL,UNIQUE(model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric));"
    "CREATE TABLE object_regions(region_id INTEGER PRIMARY KEY AUTOINCREMENT,frame_id INTEGER NOT NULL,source_id TEXT NOT NULL,capture_utc_ms INTEGER NOT NULL,bbox_x INTEGER NOT NULL,bbox_y INTEGER NOT NULL,bbox_width INTEGER NOT NULL,bbox_height INTEGER NOT NULL,image_width INTEGER NOT NULL,image_height INTEGER NOT NULL,object_label TEXT NOT NULL DEFAULT '',model_id INTEGER NOT NULL REFERENCES object_models(model_id),embedding_status INTEGER NOT NULL DEFAULT 1,crop_path TEXT,created_at INTEGER NOT NULL,frame_sha256 TEXT NOT NULL DEFAULT '',UNIQUE(frame_id,bbox_x,bbox_y,bbox_width,bbox_height,model_id));"
    "CREATE TABLE object_vectors(region_id INTEGER PRIMARY KEY REFERENCES object_regions(region_id) ON DELETE CASCADE,embedding BLOB NOT NULL);"
    "CREATE INDEX ix_obj_time ON object_regions(capture_utc_ms);CREATE INDEX ix_obj_src ON object_regions(source_id,capture_utc_ms);CREATE INDEX ix_obj_label ON object_regions(object_label);CREATE INDEX ix_obj_frame ON object_regions(frame_id);"
    "ALTER TABLE object_regions ADD COLUMN index_origin TEXT NOT NULL DEFAULT 'manual';ALTER TABLE object_regions ADD COLUMN class_id INTEGER;ALTER TABLE object_regions ADD COLUMN class_name TEXT NOT NULL DEFAULT '';"
    "ALTER TABLE object_regions ADD COLUMN confidence REAL;ALTER TABLE object_regions ADD COLUMN detector TEXT NOT NULL DEFAULT '';ALTER TABLE object_regions ADD COLUMN detector_version TEXT NOT NULL DEFAULT '';"
    "ALTER TABLE object_regions ADD COLUMN crop_bytes INTEGER NOT NULL DEFAULT 0;ALTER TABLE object_regions ADD COLUMN crop_width INTEGER NOT NULL DEFAULT 0;ALTER TABLE object_regions ADD COLUMN crop_height INTEGER NOT NULL DEFAULT 0;ALTER TABLE object_regions ADD COLUMN crop_removed_at INTEGER;"
    "CREATE INDEX ix_obj_origin ON object_regions(index_origin,class_name);"
    "CREATE TABLE object_frame_progress(frame_id INTEGER NOT NULL,detector TEXT NOT NULL,detector_version TEXT NOT NULL,source_id TEXT NOT NULL DEFAULT '',capture_utc_ms INTEGER NOT NULL,status TEXT NOT NULL,objects_found INTEGER NOT NULL DEFAULT 0,objects_indexed INTEGER NOT NULL DEFAULT 0,threshold REAL,error TEXT NOT NULL DEFAULT '',processed_at INTEGER NOT NULL,PRIMARY KEY(frame_id,detector,detector_version)) WITHOUT ROWID;"
    "CREATE INDEX ix_obj_prog_time ON object_frame_progress(capture_utc_ms);CREATE TABLE object_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);PRAGMA user_version=2;";
std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
}

TEST(ObjectSchemaV3, MigratesAV2CopyKeepingEveryRowIdVectorAndProgressAndIsolatingSources) {
    vmtest::TempDir dir; const std::string path = (fs::path(dir.str()) / "objects.db").string();
    {
        sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK); char* m = nullptr;
        ASSERT_EQ(sqlite3_exec(db, kV2Ddl, nullptr, nullptr, &m), SQLITE_OK) << (m ? m : "");
        sqlite3_exec(db, "INSERT INTO object_models VALUES(1,'mn','1','sha','pp1',4,'f32','cosine');", nullptr, nullptr, nullptr);
        for (int i = 1; i <= 5; ++i) {   // regions 1..5: camera, mixed manual/auto, one with an expired crop; then region 5 is deleted so the AUTOINCREMENT counter exceeds MAX(region_id)
            const bool autoo = i % 2 == 0;
            const std::string sql = "INSERT INTO object_regions(frame_id,source_id,capture_utc_ms,bbox_x,bbox_y,bbox_width,bbox_height,image_width,image_height,object_label,model_id,created_at,frame_sha256,index_origin,class_id,class_name,confidence,detector,detector_version,crop_path,crop_bytes,crop_width,crop_height,crop_removed_at)"
                                    " VALUES(" + std::to_string(40 + i) + ",'/dev/video1'," + std::to_string(1793900000000LL + i) + ",10,20,30,40,480,360,'" + (autoo ? "bottle" : "mine") + "',1," + std::to_string(1793900100000LL + i) + ",'sha'," +
                                    (autoo ? "'auto',39,'bottle',0.42,'yolox_s','v1','object_crops/f" + std::to_string(40 + i) + "_10_20_30_40.jpg',1234,30,40" : "'manual',NULL,'',NULL,'','',NULL,0,0,0") + "," + (i == 4 ? "1793900200000" : "NULL") + ");";
            ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &m), SQLITE_OK) << (m ? m : "");
            sqlite3_stmt* st = nullptr; sqlite3_prepare_v2(db, "INSERT INTO object_vectors VALUES(?,?);", -1, &st, nullptr);
            const float v[4] = {1.0f * i, 0.5f, 0.25f, 0.125f}; sqlite3_bind_int(st, 1, i); sqlite3_bind_blob(st, 2, v, 16, SQLITE_TRANSIENT); sqlite3_step(st); sqlite3_finalize(st);
        }
        sqlite3_exec(db, "DELETE FROM object_vectors WHERE region_id=5; DELETE FROM object_regions WHERE region_id=5;", nullptr, nullptr, nullptr);   // seq stays 5 although MAX(region_id)=4
        sqlite3_exec(db, "INSERT INTO object_frame_progress VALUES(41,'yolox_s','v1','/dev/video1',1793900000001,'ok',1,1,0.3,'',1793900300000);", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "INSERT INTO object_meta VALUES('auto_cursor_utc_ms','1793900000009');", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    const std::string before = slurp(path);
    std::string e;
    {
        ObjectIndex oi({dir.str()}); ASSERT_TRUE(oi.open(e)) << "open: " << e;
        const auto sm = oi.summary();
        EXPECT_EQ(sm.schema_version, 3); EXPECT_EQ(sm.regions, 4u); EXPECT_EQ(sm.manual_regions, 2u); EXPECT_EQ(sm.auto_regions, 2u);
        EXPECT_EQ(sm.frames_processed, 1u); EXPECT_EQ(sm.crops_expired, 1u); EXPECT_EQ(sm.crops_retained, 1u);
        const auto rows = oi.list_regions({}, 20, 0, false);
        ASSERT_EQ(rows.size(), 4u);
        for (size_t i = 0; i < 4; ++i) {                                                                  // ids and content preserved, all camera
            const auto& r = rows[i]; const int n = static_cast<int>(i) + 1;
            EXPECT_EQ(r.region_id, n); EXPECT_EQ(r.frame_id, 40 + n); EXPECT_EQ(r.source_type, "camera"); EXPECT_EQ(r.source_kind, ""); EXPECT_EQ(r.pop_session_id, "");
            EXPECT_EQ(r.index_origin, n % 2 == 0 ? "auto" : "manual"); EXPECT_EQ(r.roi.w, 30); EXPECT_EQ(r.playback_offset_ms, -1);
            if (n % 2 == 0) { EXPECT_EQ(r.class_name, "bottle"); EXPECT_NEAR(r.confidence, 0.42f, 1e-4); EXPECT_EQ(r.detector_version, "v1"); EXPECT_EQ(r.crop_bytes, 1234u); }
        }
        EXPECT_EQ(rows[3].crop_state, "expired"); EXPECT_EQ(rows[1].crop_state, "retained");
        FrameProgress p; EXPECT_TRUE(oi.get_frame_progress(41, "yolox_s", "v1", p)); EXPECT_EQ(p.status, "ok"); EXPECT_EQ(p.source_type, "camera");
        EXPECT_FALSE(oi.get_frame_progress("display", "history", 41, "yolox_s", "v1", p)) << "a display frame with the same number is another frame";
        std::string v; EXPECT_TRUE(oi.get_meta("auto_cursor_utc_ms", v)); EXPECT_EQ(v, "1793900000009");
        ObjectSearchOptions o; o.top_k = 5; o.source_type = "camera";
        const auto res = oi.search_by_region(ObjectSources{}, 1, o); ASSERT_TRUE(res.status.ok()) << res.status.message; EXPECT_EQ(res.hits.size(), 3u);
        o.source_type = "display"; EXPECT_EQ(oi.search_by_region(ObjectSources{}, 1, o).hits.size(), 0u);
    }
    // verbatim backups of both earlier states; vectors byte-identical; AUTOINCREMENT counter kept; foreign keys cascade again
    EXPECT_EQ(slurp(fs::path(dir.str()) / "objects.db.pre-v3"), before);
    sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    sqlite3_stmt* st = nullptr; sqlite3_prepare_v2(db, "SELECT region_id,embedding FROM object_vectors ORDER BY region_id;", -1, &st, nullptr);
    for (int i = 1; i <= 4; ++i) { ASSERT_EQ(sqlite3_step(st), SQLITE_ROW); const float want[4] = {1.0f * i, 0.5f, 0.25f, 0.125f}; EXPECT_EQ(std::memcmp(sqlite3_column_blob(st, 1), want, 16), 0); }
    EXPECT_NE(sqlite3_step(st), SQLITE_ROW); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "SELECT seq FROM sqlite_sequence WHERE name='object_regions';", -1, &st, nullptr); ASSERT_EQ(sqlite3_step(st), SQLITE_ROW); EXPECT_EQ(sqlite3_column_int(st, 0), 5) << "ids of deleted regions are still never reused"; sqlite3_finalize(st);
    sqlite3_close(db);
    {   // the UNIQUE key is source-aware now: the same frame number + box in another source is another object; delete cascades to the vector
        Rig r; r.open(); r.session("s1", "ad-a", "ad-a"); const int64_t h = r.history("s1", 0, display_jpeg(kRed, 200, 100), kT0 + 500);
        ObjectIndex oi({dir.str()}); ASSERT_TRUE(oi.open(e)) << e;
        DisplayObjectSource d(*r.store);
        ASSERT_EQ(oi.index_region(d, *r.prov, "history", h, Roi{200, 100, 60, 60}, "x").code, ObjectIndexResult::Code::Indexed);
        ASSERT_TRUE(oi.delete_region(2));
        sqlite3* db2 = nullptr; ASSERT_EQ(sqlite3_open(path.c_str(), &db2), SQLITE_OK);
        sqlite3_prepare_v2(db2, "SELECT COUNT(*) FROM object_vectors WHERE region_id=2;", -1, &st, nullptr); sqlite3_step(st); EXPECT_EQ(sqlite3_column_int(st, 0), 0); sqlite3_finalize(st);
        sqlite3_prepare_v2(db2, "SELECT MAX(region_id) FROM object_regions;", -1, &st, nullptr); sqlite3_step(st); EXPECT_EQ(sqlite3_column_int(st, 0), 6) << "a new region continues after the old counter"; sqlite3_finalize(st);
        sqlite3_close(db2);
    }
}

TEST(ObjectSchemaV3, ARolledBackFileOpensAgainAndAnUnknownNewerSchemaIsRefused) {
    vmtest::TempDir dir; std::string e;
    { ObjectIndex oi({dir.str()}); ASSERT_TRUE(oi.open(e)) << e; EXPECT_EQ(oi.summary().schema_version, 3); }
    { ObjectIndex oi({dir.str()}); ASSERT_TRUE(oi.open(e)) << e; }             // idempotent
    sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open((fs::path(dir.str()) / "objects.db").string().c_str(), &db), SQLITE_OK);
    sqlite3_exec(db, "PRAGMA user_version=4;", nullptr, nullptr, nullptr); sqlite3_close(db);
    ObjectIndex newer({dir.str()}); EXPECT_FALSE(newer.open(e)); EXPECT_NE(e.find("newer"), std::string::npos);
}

// ------------------------------------------------------------------------------------------------ isolation: camera and Proof-of-Play ids are not the same thing
TEST(DisplayObjects, CameraAndDisplayFramesWithTheSameNumberStayDistinctAndSearchesNeverMixThem) {
    Rig r; r.open(); r.camera_frames(3, kT0); r.session("s1", "ad-a", "ad-a");
    const int64_t h = r.history("s1", 0, display_jpeg(kRed, 200, 100), kT0 + 500);
    const int64_t c = r.checkpoint("s1", 1, display_jpeg(kRed, 200, 100), kT0 + 6000);
    r.open_objects();
    ASSERT_EQ(r.cam_ids[0], 1); ASSERT_EQ(h, 1); ASSERT_EQ(c, 1) << "the three datasets all start at 1: exactly the collision to defend against";
    std::string e;
    // same numeric id (1), same box: camera frame 1, display history frame 1, display checkpoint 1
    const Roi roi{200, 100, 60, 60};
    auto cam1 = r.oi->index_region(*r.cams, *r.prov, "", 1, Roi{20, 20, 40, 40}, "cam"); ASSERT_EQ(cam1.code, ObjectIndexResult::Code::Indexed) << cam1.message;
    auto d_h = r.oi->index_region(*r.disp, *r.prov, "history", 1, roi, "disp-h"); ASSERT_EQ(d_h.code, ObjectIndexResult::Code::Indexed) << d_h.message;
    auto d_c = r.oi->index_region(*r.disp, *r.prov, "checkpoint", 1, roi, "disp-c"); ASSERT_EQ(d_c.code, ObjectIndexResult::Code::Indexed) << d_c.message;
    EXPECT_NE(d_h.region_id, d_c.region_id) << "history frame 1 and checkpoint 1 are different frames";
    EXPECT_EQ(r.oi->index_region(*r.disp, *r.prov, "history", 1, roi, "again").code, ObjectIndexResult::Code::Duplicate);
    EXPECT_EQ(r.oi->summary("camera").regions, 1u); EXPECT_EQ(r.oi->summary("display").regions, 2u); EXPECT_EQ(r.oi->summary().regions, 3u);
    ObjectRegion g; ASSERT_TRUE(r.oi->get_region(d_h.region_id, g));
    EXPECT_EQ(g.source_type, "display"); EXPECT_EQ(g.source_kind, "history"); EXPECT_EQ(g.frame_id, 1); EXPECT_EQ(g.pop_session_id, "s1");
    EXPECT_EQ(g.expected_media_id, "ad-a"); EXPECT_EQ(g.reported_media_id, "ad-a"); EXPECT_EQ(g.playback_offset_ms, 520); EXPECT_EQ(g.time_uncertainty_ms, 252);
    EXPECT_NE(g.provenance.find("resilience_grpc"), std::string::npos); EXPECT_EQ(g.capture_utc_ms, kT0 + 500); EXPECT_EQ(g.source_id, "main_display");
    ASSERT_TRUE(r.oi->get_region(cam1.region_id, g)); EXPECT_EQ(g.source_type, "camera"); EXPECT_EQ(g.pop_session_id, ""); EXPECT_EQ(g.playback_offset_ms, -1); EXPECT_EQ(g.source_id, "cam");
    ASSERT_TRUE(r.oi->get_region(d_c.region_id, g)); EXPECT_EQ(g.time_uncertainty_ms, 275); EXPECT_EQ(g.source_kind, "checkpoint");
    // searches: the source filter decides what is searched, never the id
    ObjectSearchOptions o; o.top_k = 10;
    o.source_type = "camera"; auto rc = r.oi->search_by_region(r.srcs(), d_h.region_id, o); ASSERT_TRUE(rc.status.ok()); ASSERT_EQ(rc.hits.size(), 1u); EXPECT_EQ(rc.hits[0].region.source_type, "camera");
    o.source_type = "display"; auto rd2 = r.oi->search_by_region(r.srcs(), d_h.region_id, o); ASSERT_EQ(rd2.hits.size(), 1u); EXPECT_EQ(rd2.hits[0].region.region_id, d_c.region_id);
    o.source_type = ""; EXPECT_EQ(r.oi->search_by_region(r.srcs(), d_h.region_id, o).hits.size(), 2u);
    ObjectFilter f; f.source_type = "display"; f.source_kind = "history"; EXPECT_EQ(r.oi->list_regions(f, 10, 0, false).size(), 1u);
    f = {}; f.source_type = "display"; f.pop_session_id = "s1"; EXPECT_EQ(r.oi->list_regions(f, 10, 0, false).size(), 2u);
    f = {}; f.source_type = "camera"; f.frame_id = 1; EXPECT_EQ(r.oi->list_regions(f, 10, 0, false).size(), 1u);
    // each hit's parent frame is resolved in ITS OWN store: delete camera JPEG 1 -> camera hit missing, display hits present
    FrameDetail fd; ASSERT_TRUE(r.rd->get_frame(1, fd)); fs::remove(r.rd->absolute_path(fd.record.frame.relative_jpeg_path));
    o.source_type = ""; auto all = r.oi->search_by_region(r.srcs(), d_h.region_id, o);
    for (const auto& hit : all.hits) {
        if (hit.region.source_type == "camera") EXPECT_FALSE(hit.image_exists); else { EXPECT_TRUE(hit.image_exists); EXPECT_TRUE(hit.image_available); }
    }
    // a region's crop is regenerated from ITS source frame
    std::vector<uint8_t> jpg;
    EXPECT_EQ(r.oi->region_crop_jpeg(r.srcs(), d_h.region_id, 0, 90, jpg, e), 0) << e;
    EXPECT_EQ(r.oi->region_crop_jpeg(r.srcs(), cam1.region_id, 0, 90, jpg, e), 3) << "camera JPEG is gone";
    detail::DecodedImage img; ASSERT_TRUE(detail::decode_jpeg_rgb(r.oi->region_crop_jpeg(r.srcs(), d_h.region_id, 0, 90, jpg, e) == 0 ? jpg.data() : nullptr, jpg.size(), img, e)) << e;
    EXPECT_EQ(img.width, 60); EXPECT_EQ(img.height, 60);
}

// ------------------------------------------------------------------------------------------------ the shared worker indexes display frames
TEST(DisplayObjects, BackfillIndexesRetainedWestonFramesWithProvenanceAndBoxesInTheStoredFrame) {
    Rig r; r.open(); r.session("sa", "ad-a", "ad-a"); r.session("sb", "ad-b", "ad-a", 100000);          // sb: expected ad-b but the player reported ad-a
    // objects near the letterbox bars (x just inside 80 / just before 560) to prove the full 640x360 frame is what is analysed
    r.history("sa", 0, display_jpeg(kRed, 84, 100, 50, 120), kT0 + 500);
    r.history("sa", 1, display_jpeg(kGreen, 500, 200, 56, 56), kT0 + 1500);
    r.history("sa", 2, display_jpeg(kBlue, 300, 100), kT0 + 2500);                                                     // a "person": never indexed
    r.history("sa", 3, display_jpeg(kYellow, 300, 100), kT0 + 3500);                                                   // laptop below the 0.30 confidence
    r.checkpoint("sa", 1, display_jpeg(kRed, 250, 150, 40, 100), kT0 + 6000);
    r.history("sb", 0, display_jpeg(kRed, 300, 120, 40, 100), kT0 + 100500, "resilience_grpc", "ad-b");
    r.checkpoint("sb", 1, display_jpeg(kRed, 100, 100), kT0 + 104000, "brightscript_screenshot");                       // not Weston: excluded
    r.open_objects(); r.make_indexer(icfg(), false, false);
    BackfillRequest q; q.source = "display"; q.limit = 50; std::string err;
    ASSERT_TRUE(r.ix->request_backfill(q, err)) << err; ASSERT_TRUE(r.wait_backfill());
    const auto s = r.ix->stats();
    EXPECT_EQ(s.backfill_state, "done"); EXPECT_EQ(s.backfill_total, 6) << "6 Weston frames (5 history + ... checkpoint), the screenshot checkpoint is not a display frame";
    EXPECT_EQ(s.objects_indexed, 4u);                                  // red x3 (hist sa0, ckpt, hist sb0) + green; blue (person) and yellow (0.28) dropped
    EXPECT_GT(s.avg_embed_ms, 0.0) << "the MobileNetV2 time per crop is measured separately from detection and indexing";
    for (const auto& x : s.sources) if (x.type == "display") { EXPECT_GT(x.avg_embed_ms, 0.0); EXPECT_GT(x.avg_frame_total_ms, 0.0); EXPECT_GT(x.avg_index_ms, 0.0); } else EXPECT_EQ(x.avg_embed_ms, 0.0) << "a source that indexed nothing has no timings";
    const auto sm = r.oi->summary("display");
    EXPECT_EQ(sm.auto_regions, 4u); EXPECT_EQ(sm.frames_processed, 6u); EXPECT_EQ(sm.frames_with_objects, 4u); EXPECT_EQ(sm.crops_retained, 4u);
    EXPECT_EQ(r.oi->summary("camera").regions, 0u);
    ObjectFilter f; f.source_type = "display"; auto rows = r.oi->list_regions(f, 20, 0, false);
    ASSERT_EQ(rows.size(), 4u);
    for (const auto& g : rows) {
        EXPECT_EQ(g.source_type, "display"); EXPECT_EQ(g.index_origin, "auto"); EXPECT_EQ(g.image_width, 640); EXPECT_EQ(g.image_height, 360);
        EXPECT_TRUE(g.class_name == "bottle" || g.class_name == "cup"); EXPECT_GT(g.confidence, 0.55f);
        EXPECT_FALSE(g.pop_session_id.empty()); EXPECT_FALSE(g.provenance.empty()); EXPECT_GE(g.playback_offset_ms, 0); EXPECT_GT(g.time_uncertainty_ms, 0);
        EXPECT_EQ(g.crop_state, "retained");
        EXPECT_GE(g.roi.x, 0); EXPECT_LE(g.roi.x + g.roi.w, 640); EXPECT_LE(g.roi.y + g.roi.h, 360);
        std::vector<uint8_t> jpg; std::string e; ASSERT_EQ(r.oi->stored_crop_jpeg(g.region_id, jpg, e), 0) << e;
        detail::DecodedImage img; ASSERT_TRUE(detail::decode_jpeg_rgb(jpg.data(), jpg.size(), img, e));
        EXPECT_EQ(img.width, g.crop_width); EXPECT_EQ(img.height, g.crop_height);
        const uint8_t* c = img.rgb.data() + ((img.height / 2) * img.width + img.width / 2) * 3;                              // the crop shows the object, not the black bars
        if (g.class_name == "bottle") { EXPECT_GT(c[0], 170); EXPECT_LT(c[1], 90); } else { EXPECT_GT(c[1], 150); EXPECT_LT(c[0], 90); }
        if (g.source_kind == "history" && g.pop_session_id == "sa" && g.class_name == "bottle") { EXPECT_EQ(g.roi.x, 84); EXPECT_EQ(g.roi.w, 50); EXPECT_EQ(g.roi.h, 120); }   // box next to the pillar bar
        if (g.class_name == "cup") { EXPECT_EQ(g.roi.x, 500); EXPECT_EQ(g.roi.y, 200); EXPECT_EQ(g.roi.w, 56); }
        if (g.pop_session_id == "sb") { EXPECT_EQ(g.expected_media_id, "ad-b"); EXPECT_EQ(g.reported_media_id, "ad-a") << "the player reported another advertisement than scheduled"; }
        else { EXPECT_EQ(g.expected_media_id, "ad-a"); EXPECT_EQ(g.reported_media_id, "ad-a"); }
    }
    // progress is keyed by source + kind + id: history frame 1 and checkpoint 1 are tracked separately, and nothing is detected twice after a restart
    FrameProgress p; EXPECT_TRUE(r.oi->get_frame_progress("display", "history", 1, "mock_detector", "1", p)); EXPECT_TRUE(r.oi->get_frame_progress("display", "checkpoint", 1, "mock_detector", "1", p));
    EXPECT_FALSE(r.oi->get_frame_progress("camera", "", 1, "mock_detector", "1", p));
    const int calls = r.det->calls; r.ix.reset(); r.make_indexer(icfg(), false, false);
    ASSERT_TRUE(r.ix->request_backfill(q, err)) << err; ASSERT_TRUE(r.wait_backfill());
    EXPECT_EQ(r.det->calls, calls); EXPECT_EQ(r.ix->stats().backfill_total, 0);
}

TEST(DisplayObjects, ExpiredDisplayJpegsAreSkippedAndRetainedObjectCropsOutliveTheirParents) {
    Rig r; r.open(); r.session("sa", "ad-a", "ad-a");
    for (int k = 0; k < 4; ++k) r.history("sa", k, display_jpeg(kRed, 100 + k * 30, 100, 40, 100), kT0 + 500 + k * 1000);
    r.open_objects(); r.make_indexer(icfg(), false, false);
    BackfillRequest q; q.source = "display"; q.limit = 2; std::string err;                 // the two NEWEST frames (backfill is newest first)
    ASSERT_TRUE(r.ix->request_backfill(q, err)) << err; ASSERT_TRUE(r.wait_backfill());
    ASSERT_EQ(r.oi->summary("display").auto_regions, 2u);
    r.store->prune_history(kT0 + 10000, 1ull << 40, 0, 0);                                  // JPEG retention removes all four parent images
    ObjectFilter f; f.source_type = "display"; const auto rows = r.oi->list_regions(f, 10, 0, false);
    std::vector<uint8_t> jpg; std::string e;
    for (const auto& g : rows) {
        EXPECT_EQ(r.oi->stored_crop_jpeg(g.region_id, jpg, e), 0) << "the object crop is retained independently";
        EXPECT_EQ(r.oi->region_crop_jpeg(r.srcs(), g.region_id, 0, 90, jpg, e), 2) << "...while the parent frame has expired"; EXPECT_NE(e.find("retention"), std::string::npos);
    }
    ObjectSearchOptions o; o.top_k = 5; o.source_type = "display"; o.require_image = true;
    auto res = r.oi->search_by_region(r.srcs(), rows[0].region_id, o); ASSERT_EQ(res.hits.size(), 1u);
    EXPECT_FALSE(res.hits[0].image_available); EXPECT_TRUE(res.hits[0].crop_available);
    // expired parents are never "reconstructed": a new range backfill finds nothing to do and records nothing for them
    r.ix->stop(); r.make_indexer(icfg(), false, false);
    BackfillRequest q2; q2.source = "display"; q2.limit = 10; ASSERT_TRUE(r.ix->request_backfill(q2, err)) << err; ASSERT_TRUE(r.wait_backfill());
    EXPECT_EQ(r.ix->stats().backfill_total, 0);
    // an explicit request for an expired frame is recorded as unavailable, not detected
    BackfillRequest q3; q3.source = "display"; q3.frame_ids = {r.hist_ids[0]}; ASSERT_TRUE(r.ix->request_backfill(q3, err)) << err; ASSERT_TRUE(r.wait_backfill());
    FrameProgress p; ASSERT_TRUE(r.oi->get_frame_progress("display", "history", r.hist_ids[0], "mock_detector", "1", p)); EXPECT_EQ(p.status, "image_unavailable");
    EXPECT_EQ(r.ix->stats().frames_image_unavailable, 1u);
}

TEST(DisplayObjects, LiveScanIndexesOnlyNewlyPersistedFramesAndABackfillNeverStarvesIt) {
    Rig r; r.open(); r.camera_frames(20, kT0); r.session("sa", "ad-a", "ad-a");
    r.open_objects();
    auto c = icfg(); c.backfill_pause_ms = 120;                                           // a slow bounded backfill of the camera (20 frames ~ 2.4 s)
    r.make_indexer(c, /*live_cam=*/false, /*live_disp=*/true, 600);
    BackfillRequest q; q.source = "camera"; q.limit = 20; std::string err; ASSERT_TRUE(r.ix->request_backfill(q, err)) << err;
    ASSERT_TRUE(vmtest::wait_until([&] { return r.ix->stats().backfill_state == "running"; }, 5000));
    // frames persisted AFTER the live scan started (as Phase 2 history does during a session)
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 1000;
    const int64_t h0 = r.history("sa", 0, display_jpeg(kRed, 200, 100), now);
    r.history("sa", 1, display_jpeg(kGreen, 300, 100), now + 1000);
    ASSERT_TRUE(vmtest::wait_until([&] { FrameProgress p; return r.oi->get_frame_progress("display", "history", h0, "mock_detector", "1", p); }, 5000)) << "the live display frame must be served while the backfill runs";
    EXPECT_NE(r.ix->stats().backfill_state, "done") << "...before the backfill has finished: live work is not starved";
    ASSERT_TRUE(r.wait_backfill());
    const auto st = r.ix->stats();
    ASSERT_EQ(st.sources.size(), 2u);
    uint64_t disp_done = 0; for (const auto& s : st.sources) if (s.type == "display") disp_done = s.frames_processed;
    EXPECT_GE(disp_done, 1u);
    EXPECT_EQ(r.oi->summary("camera").frames_processed, 20u) << "the camera backfill completed";
    // nothing older than the live cursor start was touched: a frame persisted BEFORE enabling is not indexed automatically
    EXPECT_EQ(r.oi->summary("display").frames_processed, disp_done);
}

TEST(DisplayObjects, EachSourceHasItsOwnQueueAndRateLimit) {
    Rig r; r.open(); r.camera_frames(2, kT0); r.session("sa", "ad-a", "ad-a"); r.open_objects();
    std::vector<IndexerSource> v{IndexerSource{r.cams.get(), true, 6, 4}, IndexerSource{r.disp.get(), true, 600, 2}};
    r.ix = std::make_unique<ObjectIndexer>(icfg(), *r.oi, v, [&](std::string&) { return std::shared_ptr<EmbeddingProvider>(r.prov); }, [&](std::string&) { return std::shared_ptr<ObjectDetector>(r.det); });
    r.ix->start();
    ASSERT_TRUE(vmtest::wait_until([&] { return r.ix->stats().sources.size() == 2; }, 2000));
    const auto st = r.ix->stats();
    for (const auto& s : st.sources) {
        if (s.type == "camera") { EXPECT_EQ(s.frames_per_minute, 6); EXPECT_EQ(s.queue_capacity, 4); } else { EXPECT_EQ(s.frames_per_minute, 600); EXPECT_EQ(s.queue_capacity, 2); EXPECT_TRUE(s.live_scan); }
    }
    BackfillRequest bad; bad.source = "nonsense"; std::string err; EXPECT_FALSE(r.ix->request_backfill(bad, err)); EXPECT_NE(err.find("not enabled"), std::string::npos);
    EXPECT_TRUE(r.ix->has_source("display")); EXPECT_FALSE(r.ix->has_source("hdmi"));
}
