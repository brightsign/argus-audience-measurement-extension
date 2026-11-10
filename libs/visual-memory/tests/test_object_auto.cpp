// SPDX-License-Identifier: MIT
// D4 automatic object indexing: schema v2 migration, detector-driven indexing worker, crop storage/retention.
#include "visualmemory/mock_embedding_provider.h"
#include "visualmemory/object_detector.h"
#include "visualmemory/object_index.h"
#include "visualmemory/object_indexer.h"
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
constexpr int kW = 160, kH = 120;
struct Rgb { uint8_t r, g, b; };
const Rgb kObj[4] = {{220, 30, 30}, {30, 200, 40}, {30, 40, 220}, {230, 220, 30}};     // red green blue yellow
const Rgb kBg[3] = {{128, 128, 128}, {60, 70, 80}, {200, 190, 180}};

int64_t wall_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

OwnedFrame scene(Rgb bg, Rgb obj, int sx, int sy, int64_t ts, uint64_t seq) {
    OwnedFrame f; f.width = kW; f.height = kH; f.pixel_format = PixelFormat::BGR24; f.stride0 = kW * 3;
    f.pixels.resize(static_cast<size_t>(kW) * kH * 3);
    for (int y = 0; y < kH; ++y) for (int x = 0; x < kW; ++x) {
        const bool in = x >= sx && x < sx + 40 && y >= sy && y < sy + 40;
        const Rgb c = in ? obj : bg;
        uint8_t* p = f.pixels.data() + (static_cast<size_t>(y) * kW + x) * 3;
        p[0] = c.b; p[1] = c.g; p[2] = c.r;
    }
    f.metadata.source_id = "cam"; f.metadata.source_type = "file"; f.metadata.device_id = "t";
    f.metadata.capture_utc_ms = ts; f.metadata.source_sequence = seq;
    return f;
}

Config store_cfg(const std::string& root) {
    Config c; c.root_directory = root; c.stored_width = kW; c.stored_height = kH; c.jpeg_quality = 95;
    c.max_pending_frames = 16; c.retention_hours = 0; c.max_storage_bytes = 0; c.embedding.enabled = false;
    return c;
}

Roi roi_of(int i) { return {8 + ((i * 13) % 11) * 8, 8 + ((i * 7) % 7) * 8, 40, 40}; }

// Frame i holds an object of colour i%4 at roi_of(i); capture time = base + i*1000.
struct Store {
    vmtest::TempDir dir;
    std::vector<int64_t> ids;
    void build(int n, int64_t base) {
        VisualMemory vm(store_cfg(dir.str()));
        ASSERT_TRUE(vm.start());
        for (int i = 0; i < n; ++i) {
            const Roi r = roi_of(i);
            bool ok = false;
            for (int t = 0; t < 500 && !ok; ++t) {
                ok = vm.try_submit(scene(kBg[(i / 4) % 3], kObj[i % 4], r.x, r.y, base + i * 1000, static_cast<uint64_t>(i + 1)));
                if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ASSERT_TRUE(ok);
        }
        ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == static_cast<uint64_t>(n); }));
        vm.stop();
        VisualMemoryReader r({dir.str()}); std::string e; ASSERT_TRUE(r.open(e)) << e;
        for (const auto& fr : r.list_frames({}, 1000, 0, false)) ids.push_back(fr.frame.id);
        ASSERT_EQ(ids.size(), static_cast<size_t>(n));
    }
};

// Finds the saturated 40x40 square and classifies it by colour: red=bottle(39) green=cup(41) blue=person(0) yellow=laptop(63).
// yellow is reported with confidence 0.28 (below the default 0.30), blue (person) is not in the allowlist.
std::vector<DetectedObject> colour_detector(const ImageView& v) {
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
    else if (c.b > 150 && c.r < 90) { d.class_id = 0; d.confidence = 0.80f; }
    else { d.class_id = 0; d.confidence = 0.80f; }
    d.class_name = coco80_name(d.class_id);
    out.push_back(d);
    return out;
}

ObjectIndexerConfig icfg() {
    ObjectIndexerConfig c;
    c.class_ids = {39, 40, 41, 63, 67, 73};
    c.min_confidence = 0.30f; c.poll_ms = 50; c.backfill_pause_ms = 0; c.backoff_ms = 100; c.frames_per_minute = 600;
    return c;
}

struct Rig {
    Store st;
    std::unique_ptr<VisualMemoryReader> rd;
    std::unique_ptr<ObjectIndex> oi;
    std::shared_ptr<MockEmbeddingProvider> prov = std::make_shared<MockEmbeddingProvider>(32);
    std::shared_ptr<MockObjectDetector> det = std::make_shared<MockObjectDetector>(colour_detector);
    std::unique_ptr<ObjectIndexer> ix;
    void open() {
        std::string e;
        rd = std::make_unique<VisualMemoryReader>(ReaderConfig{st.dir.str()}); ASSERT_TRUE(rd->open(e)) << e;
        ObjectIndexConfig oc; oc.root_directory = st.dir.str();
        oi = std::make_unique<ObjectIndex>(oc); ASSERT_TRUE(oi->open(e)) << e;
    }
    void make_indexer(ObjectIndexerConfig c) {
        ix = std::make_unique<ObjectIndexer>(c, *oi, *rd,
            [this](std::string&) { return std::shared_ptr<EmbeddingProvider>(prov); },
            [this](std::string&) { return std::shared_ptr<ObjectDetector>(det); });
        ix->start();
    }
    bool wait_backfill(int ms = 10000) {
        return vmtest::wait_until([&] { const auto s = ix->stats().backfill_state; return s == "done" || s == "cancelled" || s == "error"; }, ms);
    }
};
} // namespace

// ---------------------------------------------------------------------------------------------- class ids
TEST(ObjectDetectorIds, CocoMappingMatchesTheModelLabelFile) {
    EXPECT_EQ(coco80_names().size(), 80u);
    EXPECT_EQ(coco80_id("person"), 0);
    EXPECT_EQ(coco80_id("bottle"), 39);
    EXPECT_EQ(coco80_id("wine glass"), 40);  EXPECT_EQ(coco80_id("wine_glass"), 40);
    EXPECT_EQ(coco80_id("cup"), 41);
    EXPECT_EQ(coco80_id("laptop"), 63);
    EXPECT_EQ(coco80_id("cell phone"), 67);  EXPECT_EQ(coco80_id("cell_phone"), 67);
    EXPECT_EQ(coco80_id("book"), 73);
    EXPECT_EQ(coco80_id("nonsense"), -1);
    EXPECT_STREQ(coco80_name(39), "bottle");
    EXPECT_STREQ(coco80_name(80), "");
    // The packaged model label list is the authority for the index order.
    const fs::path f = fs::path(__FILE__).parent_path() / ".." / ".." / ".." / "install" / "RK3588" / "model" / "coco_80_labels_list.txt";
    std::ifstream in(f);
    if (!in) GTEST_SKIP() << "label file not present: " << f;
    std::string line; size_t i = 0;
    while (std::getline(in, line)) { ASSERT_LT(i, coco80_names().size()); EXPECT_EQ(line, coco80_names()[i]) << "class " << i; ++i; }
    EXPECT_EQ(i, 80u);
}

// ---------------------------------------------------------------------------------------------- migration
namespace {
const char* kV1Ddl =
    "CREATE TABLE object_models(model_id INTEGER PRIMARY KEY,model_name TEXT NOT NULL,model_version TEXT NOT NULL,"
    " model_sha256 TEXT NOT NULL,preprocessing TEXT NOT NULL,dimension INTEGER NOT NULL,dtype TEXT NOT NULL,metric TEXT NOT NULL,"
    " UNIQUE(model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric));"
    "CREATE TABLE object_regions(region_id INTEGER PRIMARY KEY AUTOINCREMENT,frame_id INTEGER NOT NULL,"
    " source_id TEXT NOT NULL,capture_utc_ms INTEGER NOT NULL,bbox_x INTEGER NOT NULL,bbox_y INTEGER NOT NULL,"
    " bbox_width INTEGER NOT NULL,bbox_height INTEGER NOT NULL,image_width INTEGER NOT NULL,image_height INTEGER NOT NULL,"
    " object_label TEXT NOT NULL DEFAULT '',model_id INTEGER NOT NULL REFERENCES object_models(model_id),"
    " embedding_status INTEGER NOT NULL DEFAULT 1,crop_path TEXT,created_at INTEGER NOT NULL,frame_sha256 TEXT NOT NULL DEFAULT '',"
    " UNIQUE(frame_id,bbox_x,bbox_y,bbox_width,bbox_height,model_id));"
    "CREATE TABLE object_vectors(region_id INTEGER PRIMARY KEY REFERENCES object_regions(region_id) ON DELETE CASCADE,embedding BLOB NOT NULL);"
    "CREATE INDEX ix_obj_time ON object_regions(capture_utc_ms);CREATE INDEX ix_obj_src ON object_regions(source_id,capture_utc_ms);"
    "CREATE INDEX ix_obj_label ON object_regions(object_label);CREATE INDEX ix_obj_frame ON object_regions(frame_id);"
    "PRAGMA user_version=1;";

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }

void make_v1(const std::string& root) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open((fs::path(root) / "objects.db").string().c_str(), &db), SQLITE_OK);
    char* m = nullptr;
    ASSERT_EQ(sqlite3_exec(db, kV1Ddl, nullptr, nullptr, &m), SQLITE_OK) << (m ? m : "");
    ASSERT_EQ(sqlite3_exec(db, "INSERT INTO object_models VALUES(1,'mobilenet_v2_embedder','tv','abc','pp1',4,'f32','cosine');", nullptr, nullptr, &m), SQLITE_OK);
    for (int i = 1; i <= 3; ++i) {
        const std::string sql = "INSERT INTO object_regions(frame_id,source_id,capture_utc_ms,bbox_x,bbox_y,bbox_width,bbox_height,image_width,image_height,object_label,model_id,created_at,frame_sha256)"
                                " VALUES(" + std::to_string(20460 + i) + ",'/dev/video1'," + std::to_string(1793900000000LL + i) + ",10,20,30,40,640,360,'old" + std::to_string(i) + "',1," + std::to_string(1793900100000LL + i) + ",'sha');";
        ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &m), SQLITE_OK);
        sqlite3_stmt* st = nullptr; ASSERT_EQ(sqlite3_prepare_v2(db, "INSERT INTO object_vectors VALUES(?,?);", -1, &st, nullptr), SQLITE_OK);
        const float v[4] = {1.0f * i, 0.5f, 0.25f, 0.125f};
        sqlite3_bind_int(st, 1, i); sqlite3_bind_blob(st, 2, v, sizeof(v), SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(st), SQLITE_DONE); sqlite3_finalize(st);
    }
    sqlite3_close(db);
}
} // namespace

TEST(ObjectSchemaV2, MigratesV1PreservingEveryManualRecord) {
    vmtest::TempDir dir; make_v1(dir.str());
    const std::string before = slurp(fs::path(dir.str()) / "objects.db");
    std::string e;
    {
        ObjectIndex oi({dir.str()}); ASSERT_TRUE(oi.open(e)) << e;
        const auto s = oi.summary();
        EXPECT_EQ(s.schema_version, 3);
        EXPECT_EQ(s.regions, 3u); EXPECT_EQ(s.manual_regions, 3u); EXPECT_EQ(s.auto_regions, 0u);
        EXPECT_EQ(s.frames_processed, 0u); EXPECT_EQ(s.crops_retained, 0u);
        const auto rows = oi.list_regions({}, 10, 0, false);
        ASSERT_EQ(rows.size(), 3u);
        for (size_t i = 0; i < 3; ++i) {
            EXPECT_EQ(rows[i].label, "old" + std::to_string(i + 1));
            EXPECT_EQ(rows[i].index_origin, "manual"); EXPECT_EQ(rows[i].class_id, -1); EXPECT_EQ(rows[i].class_name, "");
            EXPECT_LT(rows[i].confidence, 0.0f); EXPECT_EQ(rows[i].crop_state, "none"); EXPECT_FALSE(rows[i].has_crop_file);
            EXPECT_EQ(rows[i].frame_id, 20461 + static_cast<int64_t>(i));
            EXPECT_EQ(rows[i].roi.w, 30); EXPECT_EQ(rows[i].image_width, 640);
        }
        // the old vectors still search (self-query through the stored vector)
        ObjectSearchOptions o; o.top_k = 3;
        const auto res = oi.search_by_region(nullptr, 1, o);
        ASSERT_TRUE(res.status.ok()) << res.status.message;
        EXPECT_EQ(res.hits.size(), 2u);
    }
    // a verbatim copy of the v1 file (and of the v2 state before the v3 rebuild) was kept; nothing was deleted
    ASSERT_TRUE(fs::exists(fs::path(dir.str()) / "objects.db.pre-v3"));
    ASSERT_TRUE(fs::exists(fs::path(dir.str()) / "objects.db.pre-v2"));
    EXPECT_EQ(slurp(fs::path(dir.str()) / "objects.db.pre-v2"), before);
    // vectors byte-identical after migration
    sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open_v2((fs::path(dir.str()) / "objects.db").string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* st = nullptr; sqlite3_prepare_v2(db, "SELECT region_id,embedding FROM object_vectors ORDER BY region_id;", -1, &st, nullptr);
    for (int i = 1; i <= 3; ++i) {
        ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
        const float want[4] = {1.0f * i, 0.5f, 0.25f, 0.125f};
        ASSERT_EQ(sqlite3_column_bytes(st, 1), 16);
        EXPECT_EQ(std::memcmp(sqlite3_column_blob(st, 1), want, 16), 0);
    }
    sqlite3_finalize(st); sqlite3_close(db);
    // idempotent reopen: no second migration, backup untouched
    const auto bak_time = fs::last_write_time(fs::path(dir.str()) / "objects.db.pre-v2");
    ObjectIndex again({dir.str()}); ASSERT_TRUE(again.open(e)) << e;
    EXPECT_EQ(again.summary().regions, 3u);
    EXPECT_EQ(fs::last_write_time(fs::path(dir.str()) / "objects.db.pre-v2"), bak_time);
}

TEST(ObjectSchemaV2, FreshDatabaseHasNoBackupAndNewerSchemaIsRefused) {
    vmtest::TempDir dir; std::string e;
    { ObjectIndex oi({dir.str()}); ASSERT_TRUE(oi.open(e)) << e; EXPECT_EQ(oi.summary().schema_version, 3); }
    EXPECT_FALSE(fs::exists(fs::path(dir.str()) / "objects.db.pre-v2")); EXPECT_FALSE(fs::exists(fs::path(dir.str()) / "objects.db.pre-v3"));
    sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open((fs::path(dir.str()) / "objects.db").string().c_str(), &db), SQLITE_OK);
    sqlite3_exec(db, "PRAGMA user_version=4;", nullptr, nullptr, nullptr); sqlite3_close(db);
    ObjectIndex newer({dir.str()}); EXPECT_FALSE(newer.open(e)); EXPECT_NE(e.find("newer"), std::string::npos);
}

TEST(ObjectSchemaV2, MetaAndProgressRoundTrip) {
    vmtest::TempDir dir; std::string e;
    ObjectIndex oi({dir.str()}); ASSERT_TRUE(oi.open(e)) << e;
    std::string v; EXPECT_FALSE(oi.get_meta("k", v));
    ASSERT_TRUE(oi.set_meta("k", "42")); ASSERT_TRUE(oi.get_meta("k", v)); EXPECT_EQ(v, "42");
    FrameProgress p; p.frame_id = 7; p.detector = "d"; p.detector_version = "1"; p.capture_utc_ms = 99; p.status = "ok"; p.objects_found = 2; p.objects_indexed = 2; p.threshold = 0.3f;
    ASSERT_TRUE(oi.record_frame_progress(p));
    FrameProgress q; ASSERT_TRUE(oi.get_frame_progress(7, "d", "1", q));
    EXPECT_EQ(q.status, "ok"); EXPECT_EQ(q.objects_indexed, 2); EXPECT_EQ(q.capture_utc_ms, 99); EXPECT_FLOAT_EQ(q.threshold, 0.3f);
    EXPECT_FALSE(oi.get_frame_progress(7, "d", "2", q));            // other detector version = not processed
    p.status = "error"; ASSERT_TRUE(oi.record_frame_progress(p)); ASSERT_TRUE(oi.get_frame_progress(7, "d", "1", q)); EXPECT_EQ(q.status, "error");
}

// ---------------------------------------------------------------------------------------------- indexing
TEST(ObjectAutoIndex, BackfillIndexesAllowedClassesWithProvenanceAndCrops) {
    Rig rig; rig.st.build(12, 1'700'000'000'000LL); rig.open();
    rig.make_indexer(icfg());
    BackfillRequest r; r.frame_ids = rig.st.ids; r.limit = 50;
    std::string err; ASSERT_TRUE(rig.ix->request_backfill(r, err)) << err;
    ASSERT_TRUE(rig.wait_backfill());
    const auto s = rig.ix->stats();
    EXPECT_EQ(s.backfill_state, "done"); EXPECT_EQ(s.backfill_total, 12); EXPECT_EQ(s.backfill_done, 12);
    EXPECT_EQ(s.frames_processed, 12u);
    // red (bottle) and green (cup) pass; blue is a person (not allowed); yellow is below the 0.30 threshold
    EXPECT_EQ(s.objects_indexed, 6u); EXPECT_EQ(s.frames_with_objects, 6u);
    const auto sum = rig.oi->summary();
    EXPECT_EQ(sum.auto_regions, 6u); EXPECT_EQ(sum.manual_regions, 0u);
    EXPECT_EQ(sum.frames_processed, 12u); EXPECT_EQ(sum.frames_with_objects, 6u); EXPECT_EQ(sum.crops_retained, 6u); EXPECT_GT(sum.crop_bytes, 0u);
    ASSERT_EQ(sum.by_class.size(), 2u);
    for (const auto& [name, n] : sum.by_class) { EXPECT_TRUE(name == "bottle" || name == "cup") << name; EXPECT_EQ(n, 3u); }
    for (const auto& reg : rig.oi->list_regions({}, 50, 0, false)) {
        EXPECT_EQ(reg.index_origin, "auto");
        EXPECT_TRUE(reg.class_id == 39 || reg.class_id == 41);
        EXPECT_EQ(reg.label, reg.class_name);                       // label defaults to the class
        EXPECT_GT(reg.confidence, 0.55f); EXPECT_EQ(reg.detector, "mock_detector"); EXPECT_EQ(reg.detector_version, "1");
        EXPECT_EQ(reg.crop_state, "retained"); EXPECT_GT(reg.crop_bytes, 0u); EXPECT_EQ(reg.image_width, kW);
        // the stored box is the detector box (frame index recovered from the stored capture time)
        const int idx = static_cast<int>((reg.capture_utc_ms - 1'700'000'000'000LL) / 1000);
        EXPECT_EQ(reg.roi.x, roi_of(idx).x); EXPECT_EQ(reg.roi.y, roi_of(idx).y); EXPECT_EQ(reg.roi.w, 40); EXPECT_EQ(reg.roi.h, 40);
        // crop file is a decodable JPEG of the object, colour intact
        std::vector<uint8_t> jpg; std::string e; ASSERT_EQ(rig.oi->stored_crop_jpeg(reg.region_id, jpg, e), 0) << e;
        detail::DecodedImage img; ASSERT_TRUE(detail::decode_jpeg_rgb(jpg.data(), jpg.size(), img, e));
        EXPECT_EQ(img.width, reg.crop_width); EXPECT_EQ(img.height, reg.crop_height);
        const uint8_t* c = img.rgb.data() + ((img.height / 2) * img.width + img.width / 2) * 3;
        if (reg.class_id == 39) { EXPECT_GT(c[0], 170); EXPECT_LT(c[1], 90); } else { EXPECT_GT(c[1], 150); EXPECT_LT(c[0], 90); }
    }
    // every processed frame carries progress; the empty ones are recorded as "ok, 0 objects"
    FrameProgress p; ASSERT_TRUE(rig.oi->get_frame_progress(rig.st.ids[3], "mock_detector", "1", p));
    EXPECT_EQ(p.status, "ok"); EXPECT_EQ(p.objects_found, 0);
}

TEST(ObjectAutoIndex, ProgressSurvivesRestartAndPreventsDuplicates) {
    Rig rig; rig.st.build(8, 1'700'000'000'000LL); rig.open();
    BackfillRequest r; r.frame_ids = rig.st.ids; std::string err;
    rig.make_indexer(icfg()); ASSERT_TRUE(rig.ix->request_backfill(r, err)); ASSERT_TRUE(rig.wait_backfill());
    const int calls = rig.det->calls; const auto regions = rig.oi->summary().regions;
    rig.ix.reset();                                                    // "restart": a new worker on the same databases
    rig.make_indexer(icfg()); ASSERT_TRUE(rig.ix->request_backfill(r, err)); ASSERT_TRUE(rig.wait_backfill());
    EXPECT_EQ(rig.det->calls, calls);                                  // nothing was detected again
    EXPECT_EQ(rig.oi->summary().regions, regions);
    EXPECT_EQ(rig.ix->stats().backfill_done, 8);                       // but the request still completes and reports progress
    // a range backfill skips processed frames and finds nothing to do
    BackfillRequest r2; r2.limit = 20; ASSERT_TRUE(rig.ix->request_backfill(r2, err)); ASSERT_TRUE(rig.wait_backfill());
    EXPECT_EQ(rig.ix->stats().backfill_total, 0);
    EXPECT_EQ(rig.det->calls, calls);
}

TEST(ObjectAutoIndex, ExpiredFramesAreRecordedNotReconstructed) {
    Rig rig; rig.st.build(6, 1'700'000'000'000LL); rig.open();
    // frame 0's JPEG disappears (retention); the rest stay
    FrameDetail fd; ASSERT_TRUE(rig.rd->get_frame(rig.st.ids[0], fd));
    fs::remove(rig.rd->absolute_path(fd.record.frame.relative_jpeg_path));
    rig.make_indexer(icfg());
    BackfillRequest r; r.frame_ids = rig.st.ids; std::string err; ASSERT_TRUE(rig.ix->request_backfill(r, err)); ASSERT_TRUE(rig.wait_backfill());
    FrameProgress p; ASSERT_TRUE(rig.oi->get_frame_progress(rig.st.ids[0], "mock_detector", "1", p));
    EXPECT_EQ(p.status, "image_unavailable");
    EXPECT_EQ(rig.ix->stats().frames_image_unavailable, 1u);
    // a range backfill never even lists frames whose image is gone
    BackfillRequest r2; r2.limit = 20; r2.retry_failed = true; ASSERT_TRUE(rig.ix->request_backfill(r2, err)); ASSERT_TRUE(rig.wait_backfill());
    EXPECT_EQ(rig.ix->stats().backfill_total, 0);
}

TEST(ObjectAutoIndex, CropStaysInspectableAfterTheFullFrameExpires) {
    Rig rig; rig.st.build(4, 1'700'000'000'000LL); rig.open();
    rig.make_indexer(icfg());
    BackfillRequest r; r.frame_ids = rig.st.ids; std::string err; ASSERT_TRUE(rig.ix->request_backfill(r, err)); ASSERT_TRUE(rig.wait_backfill());
    ASSERT_EQ(rig.oi->summary().auto_regions, 2u);
    for (int64_t id : rig.st.ids) {                                   // every full-frame JPEG expires
        FrameDetail fd; ASSERT_TRUE(rig.rd->get_frame(id, fd)); fs::remove(rig.rd->absolute_path(fd.record.frame.relative_jpeg_path));
    }
    ObjectSearchOptions o; o.top_k = 5; o.require_image = true;
    const auto res = rig.oi->search_by_region(rig.rd.get(), rig.oi->list_regions({}, 5, 0, false).front().region_id, o);
    ASSERT_TRUE(res.status.ok()) << res.status.message;
    ASSERT_EQ(res.hits.size(), 1u);                                   // kept: its crop is retained, although the frame is gone
    EXPECT_FALSE(res.hits[0].image_exists); EXPECT_TRUE(res.hits[0].crop_available);
    std::vector<uint8_t> jpg; std::string e; EXPECT_EQ(rig.oi->stored_crop_jpeg(res.hits[0].region.region_id, jpg, e), 0);
    EXPECT_EQ(res.hits[0].region.frame_id, res.hits[0].region.frame_id);
    EXPECT_GT(res.hits[0].region.capture_utc_ms, 0);                  // timestamp and box preserved
}

TEST(ObjectAutoIndex, FiltersCapsAndDeduplicatesDetections) {
    Rig rig; rig.st.build(3, 1'700'000'000'000LL); rig.open();
    std::atomic<int> mode{0};
    rig.det = std::make_shared<MockObjectDetector>([&](const ImageView&) {
        std::vector<DetectedObject> v; auto mk = [](int cls, float c, int x, int y, int w, int h) { DetectedObject d; d.class_id = cls; d.class_name = coco80_name(cls); d.confidence = c; d.x = x; d.y = y; d.w = w; d.h = h; return d; };
        if (mode == 0) {                                              // overlapping bottle+cup (IoU>0.7), a tiny bottle, a person, a box leaving the image
            v = {mk(39, 0.9f, 20, 20, 40, 40), mk(41, 0.8f, 22, 22, 40, 40), mk(39, 0.7f, 100, 10, 10, 10), mk(0, 0.99f, 60, 60, 40, 40), mk(73, 0.5f, 120, 80, 100, 60)};
        } else {                                                      // 6 separate valid boxes -> capped at 3
            for (int i = 0; i < 6; ++i) v.push_back(mk(39, 0.9f - 0.01f * i, 4 + (i % 3) * 50, 4 + (i / 3) * 50, 30, 30));
        }
        return v;
    });
    auto c = icfg(); c.max_objects_per_frame = 3;
    rig.make_indexer(c);
    BackfillRequest r; r.frame_ids = {rig.st.ids[0]}; std::string err; ASSERT_TRUE(rig.ix->request_backfill(r, err)); ASSERT_TRUE(rig.wait_backfill());
    auto regs = rig.oi->list_regions({}, 20, 0, false);
    ASSERT_EQ(regs.size(), 2u);                                       // bottle (the cup duplicate, tiny box and person are dropped) + book clamped to the image
    std::sort(regs.begin(), regs.end(), [](const auto& a, const auto& b) { return a.class_id < b.class_id; });
    EXPECT_EQ(regs[0].class_name, "bottle"); EXPECT_NEAR(regs[0].confidence, 0.9f, 1e-4);
    EXPECT_EQ(regs[1].class_name, "book"); EXPECT_LE(regs[1].roi.x + regs[1].roi.w, kW); EXPECT_LE(regs[1].roi.y + regs[1].roi.h, kH);
    mode = 1;
    BackfillRequest r2; r2.frame_ids = {rig.st.ids[1]}; ASSERT_TRUE(rig.ix->request_backfill(r2, err)); ASSERT_TRUE(rig.wait_backfill());
    EXPECT_EQ(rig.oi->list_regions({}, 20, 0, false, nullptr).size(), 5u);   // +3, not +6
}

TEST(ObjectAutoIndex, DetectorFailureBacksOffWithoutMarkingFramesThenRecovers) {
    Rig rig; rig.st.build(4, 1'700'000'000'000LL); rig.open();
    rig.det->set_fail(true);
    auto c = icfg(); c.backoff_ms = 150;
    rig.make_indexer(c);
    BackfillRequest r; r.frame_ids = rig.st.ids; std::string err; ASSERT_TRUE(rig.ix->request_backfill(r, err));
    ASSERT_TRUE(vmtest::wait_until([&] { return rig.ix->stats().detector_errors >= 2; }, 5000));   // still below the 3-attempt limit
    const auto s = rig.ix->stats();
    EXPECT_FALSE(s.last_error.empty()); EXPECT_GT(s.last_error_utc_ms, 0);
    FrameProgress p; for (int64_t id : rig.st.ids) EXPECT_FALSE(rig.oi->get_frame_progress(id, "mock_detector", "1", p));   // no frame was marked
    rig.det->set_fail(false);                                         // recovers on its own
    ASSERT_TRUE(rig.wait_backfill(10000));
    EXPECT_EQ(rig.oi->summary().auto_regions, 2u);
}

TEST(ObjectAutoIndex, BackfillIsBoundedSingleAndCancellable) {
    Rig rig; rig.st.build(12, 1'700'000'000'000LL); rig.open();
    auto c = icfg(); c.max_backfill_frames = 5; c.backfill_pause_ms = 40;
    rig.make_indexer(c);
    BackfillRequest r; r.limit = 1000; std::string err;
    ASSERT_TRUE(rig.ix->request_backfill(r, err));
    EXPECT_FALSE(rig.ix->request_backfill(r, err));                   // one at a time
    EXPECT_NE(err.find("already"), std::string::npos);
    ASSERT_TRUE(vmtest::wait_until([&] { return rig.ix->stats().backfill_state == "running"; }, 3000));
    EXPECT_EQ(rig.ix->stats().backfill_total, 5);                     // hard cap
    rig.ix->cancel_backfill();
    ASSERT_TRUE(rig.wait_backfill());
    EXPECT_EQ(rig.ix->stats().backfill_state, "cancelled");
    EXPECT_LE(rig.ix->stats().frames_processed, 5u);
    BackfillRequest bad; bad.start_utc_ms = 10; bad.end_utc_ms = 5; EXPECT_FALSE(rig.ix->request_backfill(bad, err));
}

TEST(ObjectAutoIndex, LiveScanIsRateLimitedBoundedAndStartsAfterEnabling) {
    const int64_t future = wall_ms() + 3'600'000;
    Rig rig; rig.st.build(12, future); rig.open();
    // an "old" frame set would not be touched: only frames newer than the cursor (set when the scan first runs) are candidates
    auto c = icfg(); c.queue_depth = 2;
    rig.make_indexer(c);
    ASSERT_TRUE(vmtest::wait_until([&] { return rig.ix->stats().frames_processed >= 2; }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto s = rig.ix->stats();
    EXPECT_EQ(s.frames_processed, 2u);                                // the newest two
    EXPECT_EQ(s.skipped_stale, 10u);                                  // 10 frames were sampled out, counted, not hidden
    EXPECT_EQ(s.queue_capacity, 2);
    EXPECT_GT(s.cursor_utc_ms, 0);
    FrameProgress p; EXPECT_TRUE(rig.oi->get_frame_progress(rig.st.ids[11], "mock_detector", "1", p));
    EXPECT_FALSE(rig.oi->get_frame_progress(rig.st.ids[0], "mock_detector", "1", p));
    // the cursor is persisted: a new worker does not re-offer processed frames
    rig.ix.reset();
    const int calls = rig.det->calls;
    rig.make_indexer(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(rig.det->calls, calls);
}

TEST(ObjectAutoIndex, StopIsSafeAndIdempotent) {
    Rig rig; rig.st.build(4, 1'700'000'000'000LL); rig.open();
    auto c = icfg(); c.backfill_pause_ms = 100;
    rig.make_indexer(c);
    BackfillRequest r; r.frame_ids = rig.st.ids; std::string err; ASSERT_TRUE(rig.ix->request_backfill(r, err));
    rig.ix->stop(); rig.ix->stop();
    EXPECT_EQ(rig.ix->stats().state, "stopped");
    EXPECT_FALSE(rig.ix->request_backfill(r, err));                   // not accepted once stopped
}

// ---------------------------------------------------------------------------------------------- filters, crops, retention
TEST(ObjectAutoIndex, SearchAndListFilterByOriginAndClass) {
    Rig rig; rig.st.build(12, 1'700'000'000'000LL); rig.open();
    // one manual region (a green square) and the automatic ones
    ASSERT_EQ(rig.oi->index_region(*rig.rd, *rig.prov, rig.st.ids[1], roi_of(1), "mine").code, ObjectIndexResult::Code::Indexed);
    rig.make_indexer(icfg());
    BackfillRequest r; r.frame_ids = rig.st.ids; std::string err; ASSERT_TRUE(rig.ix->request_backfill(r, err)); ASSERT_TRUE(rig.wait_backfill());
    // frame 1 holds a manual box and (same box, same model) the automatic duplicate -> the manual record is kept
    const auto sum = rig.oi->summary();
    EXPECT_EQ(sum.manual_regions, 1u); EXPECT_EQ(sum.auto_regions, 5u);
    ObjectFilter f; f.index_origin = "auto"; uint64_t total = 0;
    EXPECT_EQ(rig.oi->list_regions(f, 50, 0, false, &total).size(), 5u); EXPECT_EQ(total, 5u);
    f.index_origin = "manual"; EXPECT_EQ(rig.oi->list_regions(f, 50, 0, false, &total).size(), 1u);
    f = {}; f.class_name = "bottle"; EXPECT_EQ(rig.oi->list_regions(f, 50, 0, false).size(), 3u);
    ObjectSearchOptions o; o.top_k = 20;
    const int64_t bottle = rig.oi->list_regions(f, 1, 0, false).front().region_id;
    auto all = rig.oi->search_by_region(rig.rd.get(), bottle, o); ASSERT_TRUE(all.status.ok());
    EXPECT_EQ(all.hits.size(), 5u);
    o.class_name = "bottle"; auto b = rig.oi->search_by_region(rig.rd.get(), bottle, o); EXPECT_EQ(b.hits.size(), 2u);
    for (const auto& h : b.hits) EXPECT_EQ(h.region.class_name, "bottle");
    o.class_name.clear(); o.index_origin = "manual"; auto m = rig.oi->search_by_region(rig.rd.get(), bottle, o);
    ASSERT_EQ(m.hits.size(), 1u); EXPECT_EQ(m.hits[0].region.index_origin, "manual"); EXPECT_EQ(m.hits[0].region.label, "mine");
    // similar bottles rank above the unrelated cups (the mock embedding is colour driven)
    o.index_origin.clear(); o.top_k = 5;
    auto ranked = rig.oi->search_by_region(rig.rd.get(), bottle, o);
    ASSERT_GE(ranked.hits.size(), 2u);
    EXPECT_EQ(ranked.hits[0].region.class_name, "bottle"); EXPECT_EQ(ranked.hits[1].region.class_name, "bottle");
    EXPECT_GT(ranked.hits[0].similarity, ranked.hits.back().similarity);
}

TEST(ObjectCropRetention, ManualRegionsAlsoKeepACropAndQuotaEvictsOldestFirst) {
    Rig rig; rig.st.build(8, 1'700'000'000'000LL); rig.open();
    std::vector<int64_t> reg;
    for (int i = 0; i < 6; ++i) {
        const auto res = rig.oi->index_region(*rig.rd, *rig.prov, rig.st.ids[i], roi_of(i), "m" + std::to_string(i));
        ASSERT_EQ(res.code, ObjectIndexResult::Code::Indexed) << res.message; reg.push_back(res.region_id);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));            // distinct created_at
    }
    auto sum = rig.oi->summary(); ASSERT_EQ(sum.crops_retained, 6u);
    const uint64_t one = rig.oi->list_regions({}, 1, 0, false).front().crop_bytes;
    // quota for ~3 crops: the three OLDEST are removed
    const auto pr = rig.oi->prune_crops(0, sum.crop_bytes - 3 * one + 1);
    EXPECT_GE(pr.removed, 3u); EXPECT_GT(pr.bytes_freed, 0u);
    sum = rig.oi->summary(); EXPECT_EQ(sum.crops_expired, pr.removed); EXPECT_EQ(sum.crops_retained, 6u - pr.removed); EXPECT_EQ(sum.regions, 6u);
    std::vector<uint8_t> jpg; std::string e;
    EXPECT_EQ(rig.oi->stored_crop_jpeg(reg[0], jpg, e), 2); EXPECT_NE(e.find("expired"), std::string::npos);
    EXPECT_EQ(rig.oi->stored_crop_jpeg(reg[5], jpg, e), 0);
    EXPECT_FALSE(fs::exists(fs::path(rig.st.dir.str()) / "object_crops" / (std::to_string(reg[0]) + ".jpg")));
    ObjectRegion r0; ASSERT_TRUE(rig.oi->get_region(reg[0], r0));
    EXPECT_EQ(r0.crop_state, "expired"); EXPECT_EQ(r0.label, "m0"); EXPECT_GT(r0.capture_utc_ms, 0); EXPECT_EQ(r0.roi.w, 40);   // everything else kept
    // age based: everything created before the cutoff (the future) goes
    const auto pa = rig.oi->prune_crops(wall_ms() + 1000, 0);
    EXPECT_EQ(rig.oi->summary().crops_retained, 0u); EXPECT_GT(pa.removed, 0u);
    EXPECT_EQ(rig.oi->summary().regions, 6u);
}

TEST(ObjectCropRetention, DeletingOrPruningRegionsRemovesTheirCropFiles) {
    Rig rig; rig.st.build(4, 1'700'000'000'000LL); rig.open();
    std::vector<int64_t> reg;
    for (int i = 0; i < 4; ++i) { const auto res = rig.oi->index_region(*rig.rd, *rig.prov, rig.st.ids[i], roi_of(i), ""); ASSERT_EQ(res.code, ObjectIndexResult::Code::Indexed); reg.push_back(res.region_id); std::this_thread::sleep_for(std::chrono::milliseconds(3)); }
    auto crop = [&](int64_t id) { return fs::path(rig.st.dir.str()) / "object_crops" / (std::to_string(id) + ".jpg"); };
    for (int64_t id : reg) ASSERT_TRUE(fs::exists(crop(id)));
    ASSERT_TRUE(rig.oi->delete_region(reg[0])); EXPECT_FALSE(fs::exists(crop(reg[0])));
    EXPECT_EQ(rig.oi->prune(0, 2), 1u);                                         // max_regions=2 -> the oldest remaining region goes
    EXPECT_FALSE(fs::exists(crop(reg[1]))); EXPECT_TRUE(fs::exists(crop(reg[2]))); EXPECT_TRUE(fs::exists(crop(reg[3])));
    // no stray temp files
    for (const auto& e : fs::directory_iterator(fs::path(rig.st.dir.str()) / "object_crops")) EXPECT_NE(e.path().extension(), ".tmp");
}

TEST(ObjectCropRetention, CropStorageCanBeDisabledAndMissingFilesReportAccurately) {
    Rig rig; rig.st.build(3, 1'700'000'000'000LL); rig.open();
    ObjectIndexConfig oc; oc.root_directory = rig.st.dir.str(); oc.store_crops = false;
    ObjectIndex nocrop(oc); std::string e; ASSERT_TRUE(nocrop.open(e)) << e;
    const auto res = nocrop.index_region(*rig.rd, *rig.prov, rig.st.ids[0], roi_of(0), "x");
    ASSERT_EQ(res.code, ObjectIndexResult::Code::Indexed); EXPECT_EQ(res.message, "");
    std::vector<uint8_t> jpg; EXPECT_EQ(nocrop.stored_crop_jpeg(res.region_id, jpg, e), 2); EXPECT_NE(e.find("no crop"), std::string::npos);
    // retained row whose file vanished (manual deletion / SD problem): reported as missing, not as a valid image
    const auto r2 = rig.oi->index_region(*rig.rd, *rig.prov, rig.st.ids[1], roi_of(1), "y"); ASSERT_EQ(r2.code, ObjectIndexResult::Code::Indexed);
    fs::remove(fs::path(rig.st.dir.str()) / "object_crops" / (std::to_string(r2.region_id) + ".jpg"));
    EXPECT_EQ(rig.oi->stored_crop_jpeg(r2.region_id, jpg, e), 3);
    EXPECT_EQ(rig.oi->stored_crop_jpeg(424242, jpg, e), 1);
    ObjectSearchOptions o; o.top_k = 5; o.require_image = true;
    const auto sr = rig.oi->search_by_region(rig.rd.get(), r2.region_id, o);
    ASSERT_EQ(sr.hits.size(), 1u);                                              // the other region (its parent frame is retained)
    const auto sx = rig.oi->search_by_region(rig.rd.get(), res.region_id, o);   // from the other side: r2's frame is retained, its crop is NOT
    ASSERT_EQ(sx.hits.size(), 1u); EXPECT_TRUE(sx.hits[0].image_exists); EXPECT_FALSE(sx.hits[0].crop_available);
}

TEST(ObjectAutoIndex, AFrameThatKeepsFailingIsMarkedAfterThreeAttempts) {
    Rig rig; rig.st.build(2, 1'700'000'000'000LL); rig.open();
    rig.det->set_fail(true);
    auto c = icfg(); c.backoff_ms = 50;
    rig.make_indexer(c);
    BackfillRequest r; r.frame_ids = {rig.st.ids[0]}; std::string err; ASSERT_TRUE(rig.ix->request_backfill(r, err));
    ASSERT_TRUE(rig.wait_backfill());
    FrameProgress p; ASSERT_TRUE(rig.oi->get_frame_progress(rig.st.ids[0], "mock_detector", "1", p));
    EXPECT_EQ(p.status, "error"); EXPECT_NE(p.error.find("3 attempts"), std::string::npos);
    EXPECT_EQ(rig.ix->stats().frames_failed, 1u); EXPECT_EQ(rig.ix->stats().detector_errors, 3u);
}

// ---------------------------------------------------------------------------------------------- benchmark (host; not run by default)
// ./visualmemory_tests --gtest_also_run_disabled_tests --gtest_filter='ObjectBench.*'
TEST(ObjectBench, DISABLED_IndexFrameCommitAndSearchLatency) {
    Rig rig; rig.st.build(1, 1'700'000'000'000LL); rig.open();
    std::vector<double> ms;
    for (int i = 0; i < 200; ++i) {                                   // 200 frames' worth: 1 object each, one commit per call
        std::vector<ObjectIndex::FrameRegion> regs;
        DetectionInfo di; di.class_id = 39; di.class_name = "bottle"; di.confidence = 0.9f; di.detector = "bench"; di.detector_version = "1";
        regs.push_back({Roi{i % 100, (i / 100) * 20, 40, 40}, di});
        FrameProgress p; p.frame_id = rig.st.ids[0]; p.detector = "bench"; p.detector_version = "i" + std::to_string(i); p.threshold = 0.3f;
        const auto r = rig.oi->index_frame(*rig.rd, *rig.prov, rig.st.ids[0], regs, p, "k", std::to_string(i));
        ASSERT_TRUE(r.ok()) << r.message; ms.push_back(r.elapsed_ms);
    }
    std::sort(ms.begin(), ms.end());
    std::printf("[bench] index_frame (decode+crop+embed(mock)+1 commit, tmp fs): p50 %.2f ms  p95 %.2f ms  max %.2f ms  (n=%zu)\n", ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back(), ms.size());
    // search scaling with 1280-d vectors inserted directly
    const int dim = 1280;
    for (int target : {1000, 10000, 50000}) {
        sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open((fs::path(rig.st.dir.str()) / "objects.db").string().c_str(), &db), SQLITE_OK);
        sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
        sqlite3_stmt* q = nullptr; sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM object_regions;", -1, &q, nullptr); sqlite3_step(q); const int have = sqlite3_column_int(q, 0); sqlite3_finalize(q);
        EmbeddingSpec spec = rig.prov->spec(); spec.dimension = dim;
        const std::string ms_sql = "INSERT OR IGNORE INTO object_models(model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric) VALUES('" + spec.model_name + "','" + spec.model_version + "','" + spec.model_sha256 + "','" + spec.preprocessing + "',1280,'" + spec.dtype + "','" + spec.metric + "');";
        sqlite3_exec(db, ms_sql.c_str(), nullptr, nullptr, nullptr);
        sqlite3_prepare_v2(db, "SELECT model_id FROM object_models WHERE dimension=1280;", -1, &q, nullptr); sqlite3_step(q); const int mid = sqlite3_column_int(q, 0); sqlite3_finalize(q);
        std::vector<float> v(dim);
        for (int i = have; i < target; ++i) {
            const std::string sql = "INSERT INTO object_regions(frame_id,source_id,capture_utc_ms,bbox_x,bbox_y,bbox_width,bbox_height,image_width,image_height,object_label,model_id,created_at,index_origin,class_id,class_name,confidence) VALUES(1,'c'," + std::to_string(1000 + i) + "," + std::to_string(i) + ",0,30,30,480,360,'bottle'," + std::to_string(mid) + ",1,'auto',39,'bottle',0.9);";
            sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
            for (int k = 0; k < dim; ++k) v[k] = static_cast<float>(((i * 31 + k * 17) % 1000) - 500) / 500.0f;
            sqlite3_stmt* ins = nullptr; sqlite3_prepare_v2(db, "INSERT INTO object_vectors VALUES(last_insert_rowid(),?);", -1, &ins, nullptr);
            sqlite3_bind_blob(ins, 1, v.data(), dim * 4, SQLITE_TRANSIENT); sqlite3_step(ins); sqlite3_finalize(ins);
        }
        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr); sqlite3_close(db);
        // the 32-d mock vectors of the first 200 do not match 1280-d queries (skipped as a different model): this measures the scan of the rest
        std::vector<float> qv(dim, 0.01f);
        ObjectSearchOptions o; o.top_k = 10;
        std::vector<double> t;
        for (int r = 0; r < 5; ++r) { rig.oi->search_by_vector(rig.rd.get(), qv, spec, o); }
        const auto res = rig.oi->search_by_vector(rig.rd.get(), qv, spec, o);
        std::printf("[bench] search %6d objects (1280-d): status=%s scanned=%llu %.1f ms\n", target, res.status.message.c_str(), static_cast<unsigned long long>(res.index_size), res.status.elapsed_ms);
    }
}
