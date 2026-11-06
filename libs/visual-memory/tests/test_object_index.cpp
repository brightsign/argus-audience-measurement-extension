// SPDX-License-Identifier: MIT
// ObjectIndex (D4): manual ROI object regions, separate vector space, search.
#include "visualmemory/mock_embedding_provider.h"
#include "visualmemory/object_index.h"
#include "visualmemory/reader.h"
#include "visualmemory/visual_memory.h"
#include "jpeg_decoder.h"
#include "test_util.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <limits>
#include <sqlite3.h>

using namespace visualmemory;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kBase = 1'767'261'600'000LL;
constexpr int kW = 160, kH = 120;
struct Rgb { uint8_t r, g, b; };

// 160x120 scene: plain background + one 40x40 coloured square at (sx,sy).
OwnedFrame scene(Rgb bg, Rgb obj, int sx, int sy, int64_t ts, uint64_t seq, const std::string& src = "cam") {
    OwnedFrame f; f.width = kW; f.height = kH; f.pixel_format = PixelFormat::BGR24; f.stride0 = kW * 3;
    f.pixels.resize(static_cast<size_t>(kW) * kH * 3);
    for (int y = 0; y < kH; ++y) for (int x = 0; x < kW; ++x) {
        const bool in = x >= sx && x < sx + 40 && y >= sy && y < sy + 40;
        const Rgb c = in ? obj : bg;
        uint8_t* p = f.pixels.data() + (static_cast<size_t>(y) * kW + x) * 3;
        p[0] = c.b; p[1] = c.g; p[2] = c.r;
    }
    f.metadata.source_id = src; f.metadata.source_type = "file"; f.metadata.device_id = "t";
    f.metadata.capture_utc_ms = ts; f.metadata.source_sequence = seq;
    return f;
}

Config cfg(const std::string& root) {
    Config c; c.root_directory = root; c.stored_width = kW; c.stored_height = kH; c.jpeg_quality = 95;
    c.max_pending_frames = 16; c.retention_hours = 0; c.max_storage_bytes = 0; c.embedding.enabled = false;
    return c;
}

// kinds: 0 red, 1 green, 2 blue, 3 yellow; backgrounds vary per frame.
const Rgb kObj[4] = {{220, 30, 30}, {30, 200, 40}, {30, 40, 220}, {230, 220, 30}};
const Rgb kBg[3] = {{128, 128, 128}, {60, 70, 80}, {200, 190, 180}};

struct Fixture {
    vmtest::TempDir dir;
    std::vector<int64_t> frame_ids;        // frame ids in submission order
    // frames 0..11: object kind (i%4) on background (i/4)%3, at varying positions
    void build(int n = 12) {
        VisualMemory vm(cfg(dir.str()));
        ASSERT_TRUE(vm.start());
        for (int i = 0; i < n; ++i) {
            auto fr = scene(kBg[(i / 4) % 3], kObj[i % 4], 8 + ((i * 13) % 11) * 8, 8 + ((i * 7) % 7) * 8, kBase + i * 1000, i + 1);
            bool ok = false;
            for (int t = 0; t < 500 && !ok; ++t) { ok = vm.try_submit(std::move(fr)); if (!ok) { fr = scene(kBg[(i / 4) % 3], kObj[i % 4], 8 + ((i * 13) % 11) * 8, 8 + ((i * 7) % 7) * 8, kBase + i * 1000, i + 1); std::this_thread::sleep_for(std::chrono::milliseconds(5)); } }
            ASSERT_TRUE(ok);
        }
        ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == static_cast<uint64_t>(n); }));
        vm.stop();
        VisualMemoryReader r({dir.str()}); std::string e; ASSERT_TRUE(r.open(e)) << e;
        for (const auto& fr : r.list_frames({}, 100, 0, false)) frame_ids.push_back(fr.frame.id);
        ASSERT_EQ(frame_ids.size(), static_cast<size_t>(n));
    }
    static Roi roi_of(int i) { return {8 + ((i * 13) % 11) * 8, 8 + ((i * 7) % 7) * 8, 40, 40}; }
};
} // namespace

TEST(ObjectRoi, ValidationRules) {
    EXPECT_EQ(validate_roi({0, 0, 160, 120}, 160, 120), RoiCheck::Ok);
    EXPECT_EQ(validate_roi({10, 10, 16, 16}, 160, 120), RoiCheck::Ok);
    EXPECT_EQ(validate_roi({10, 10, 15, 40}, 160, 120), RoiCheck::TooSmall);
    EXPECT_EQ(validate_roi({-1, 0, 40, 40}, 160, 120), RoiCheck::Invalid);
    EXPECT_EQ(validate_roi({0, 0, 0, 40}, 160, 120), RoiCheck::Invalid);
    EXPECT_EQ(validate_roi({130, 0, 40, 40}, 160, 120), RoiCheck::OutOfBounds);
    EXPECT_EQ(validate_roi({0, 100, 40, 40}, 160, 120), RoiCheck::OutOfBounds);
    EXPECT_EQ(validate_roi({0, 0, 2147483647, 40}, 160, 120), RoiCheck::OutOfBounds);   // overflow-safe
    EXPECT_EQ(validate_roi({2147483647, 0, 2147483647, 40}, 160, 120), RoiCheck::OutOfBounds);
}

TEST(ObjectIndex, CropColourAndOriginalUntouched) {
    Fixture fx; fx.build(4);
    VisualMemoryReader r({fx.dir.str()}); std::string e; ASSERT_TRUE(r.open(e));
    ObjectIndex oi({fx.dir.str()}); ASSERT_TRUE(oi.open(e)) << e;
    std::vector<uint8_t> before; ASSERT_EQ(r.read_jpeg(fx.frame_ids[0], before, e), 0);
    std::vector<uint8_t> jpg;
    ASSERT_EQ(oi.crop_jpeg(r, fx.frame_ids[0], Fixture::roi_of(0), 0, 95, jpg, e), 0) << e;
    detail::DecodedImage img; ASSERT_TRUE(detail::decode_jpeg_rgb(jpg.data(), jpg.size(), img, e));
    EXPECT_EQ(img.width, 40); EXPECT_EQ(img.height, 40);
    const uint8_t* c = img.rgb.data() + (20 * 40 + 20) * 3;            // centre pixel must be the RED object (RGB order)
    EXPECT_GT(c[0], 180); EXPECT_LT(c[1], 80); EXPECT_LT(c[2], 80);
    // scaled crop keeps aspect ratio
    ASSERT_EQ(oi.crop_jpeg(r, fx.frame_ids[0], {10, 10, 80, 40}, 40, 90, jpg, e), 0);
    ASSERT_TRUE(detail::decode_jpeg_rgb(jpg.data(), jpg.size(), img, e));
    EXPECT_EQ(img.width, 40); EXPECT_EQ(img.height, 20);
    // original frame bytes unchanged
    std::vector<uint8_t> after; ASSERT_EQ(r.read_jpeg(fx.frame_ids[0], after, e), 0);
    EXPECT_EQ(before, after);
    EXPECT_EQ(oi.crop_jpeg(r, fx.frame_ids[0], {150, 0, 40, 40}, 0, 90, jpg, e), 4);
    EXPECT_EQ(oi.crop_jpeg(r, 9999, {0, 0, 40, 40}, 0, 90, jpg, e), 1);
}

TEST(ObjectIndex, IndexDuplicatePersistAndRetrieval) {
    Fixture fx; fx.build(12);
    std::string e;
    {
        VisualMemoryReader r({fx.dir.str()}); ASSERT_TRUE(r.open(e));
        ObjectIndex oi({fx.dir.str()}); ASSERT_TRUE(oi.open(e)) << e;
        MockEmbeddingProvider prov(32);
        // index frames 0..7 (two of each colour on different backgrounds); labels by colour
        const char* names[4] = {"red", "green", "blue", "yellow"};
        for (int i = 0; i < 8; ++i) {
            auto res = oi.index_region(r, prov, fx.frame_ids[i], Fixture::roi_of(i), names[i % 4]);
            ASSERT_EQ(res.code, ObjectIndexResult::Code::Indexed) << res.message;
        }
        // duplicate -> same id, no new row
        auto a = oi.index_region(r, prov, fx.frame_ids[0], Fixture::roi_of(0), "");
        EXPECT_EQ(a.code, ObjectIndexResult::Code::Duplicate);
        EXPECT_EQ(oi.summary().regions, 8u);
        // invalid ROIs rejected
        EXPECT_EQ(oi.index_region(r, prov, fx.frame_ids[0], {150, 0, 40, 40}, "").code, ObjectIndexResult::Code::InvalidRoi);
        EXPECT_EQ(oi.index_region(r, prov, fx.frame_ids[0], {0, 0, 4, 4}, "").code, ObjectIndexResult::Code::InvalidRoi);
        EXPECT_EQ(oi.index_region(r, prov, 424242, {0, 0, 40, 40}, "").code, ObjectIndexResult::Code::FrameNotFound);
        EXPECT_EQ(oi.index_region(r, prov, fx.frame_ids[0], {0, 0, 40, 40}, std::string(200, 'x')).code, ObjectIndexResult::Code::InvalidArgument);
    }
    // persistence across restart
    VisualMemoryReader r({fx.dir.str()}); ASSERT_TRUE(r.open(e));
    ObjectIndex oi({fx.dir.str()}); ASSERT_TRUE(oi.open(e)) << e;
    EXPECT_EQ(oi.summary().regions, 8u);
    EXPECT_EQ(oi.summary().schema_version, 1);
    MockEmbeddingProvider prov(32);
    // query: unindexed frames 8..11 (colour = i%4, third background) -> top-1 must have the same colour label
    const char* names[4] = {"red", "green", "blue", "yellow"};
    for (int i = 8; i < 12; ++i) {
        ObjectSearchOptions o; o.top_k = 3;
        auto res = oi.search_by_roi(r, prov, fx.frame_ids[i], Fixture::roi_of(i), o);
        ASSERT_TRUE(res.status.ok()) << res.status.message;
        ASSERT_EQ(res.hits.size(), 3u);
        EXPECT_EQ(res.hits[0].region.label, names[i % 4]) << "query " << i;
        EXPECT_GE(res.hits[0].similarity, res.hits[1].similarity);
        EXPECT_GE(res.hits[1].similarity, res.hits[2].similarity);
        EXPECT_EQ(res.hits[0].region.roi.w, 40);
        EXPECT_TRUE(res.hits[0].image_exists);
    }
    // label filter + stored-region query excludes itself + time filter
    ObjectSearchOptions o; o.top_k = 10; o.label = "red";
    auto rr = oi.search_by_roi(r, prov, fx.frame_ids[8], Fixture::roi_of(8), o);
    ASSERT_TRUE(rr.status.ok()); EXPECT_EQ(rr.hits.size(), 2u);
    for (auto& h : rr.hits) EXPECT_EQ(h.region.label, "red");
    ObjectSearchOptions so; so.top_k = 20;
    auto self = oi.search_by_region(&r, 1, so);
    ASSERT_TRUE(self.status.ok());
    for (auto& h : self.hits) EXPECT_NE(h.region.region_id, 1);
    EXPECT_EQ(self.hits.size(), 7u);
    ObjectSearchOptions to; to.top_k = 20; to.start_utc_ms = kBase + 4000;
    auto timed = oi.search_by_roi(r, prov, fx.frame_ids[8], Fixture::roi_of(8), to);
    EXPECT_EQ(timed.hits.size(), 4u);
}

TEST(ObjectIndex, UploadedCropSearchAndEmptyIndex) {
    Fixture fx; fx.build(4);
    std::string e;
    VisualMemoryReader r({fx.dir.str()}); ASSERT_TRUE(r.open(e));
    ObjectIndex oi({fx.dir.str()}); ASSERT_TRUE(oi.open(e));
    MockEmbeddingProvider prov(32);
    std::vector<uint8_t> jpg; ASSERT_EQ(oi.crop_jpeg(r, fx.frame_ids[2], Fixture::roi_of(2), 0, 95, jpg, e), 0);
    auto empty = oi.search_by_jpeg(&r, prov, jpg, {});
    EXPECT_TRUE(empty.status.ok()); EXPECT_TRUE(empty.hits.empty());
    EXPECT_NE(empty.status.message.find("empty"), std::string::npos);
    ASSERT_TRUE(oi.index_region(r, prov, fx.frame_ids[2], Fixture::roi_of(2), "blue").ok());
    auto hit = oi.search_by_jpeg(&r, prov, jpg, {});
    ASSERT_EQ(hit.hits.size(), 1u);
    EXPECT_GT(hit.hits[0].similarity, 0.99f);
    std::vector<uint8_t> bad{1, 2, 3};
    EXPECT_EQ(oi.search_by_jpeg(&r, prov, bad, {}).status.code, SearchStatus::Code::InvalidArgument);
}

TEST(ObjectIndex, ModelMismatchRejected) {
    Fixture fx; fx.build(4);
    std::string e;
    VisualMemoryReader r({fx.dir.str()}); ASSERT_TRUE(r.open(e));
    ObjectIndex oi({fx.dir.str()}); ASSERT_TRUE(oi.open(e));
    MockEmbeddingProvider v1(32, "v1"), v2(32, "v2");
    ASSERT_TRUE(oi.index_region(r, v1, fx.frame_ids[0], Fixture::roi_of(0), "x").ok());
    auto res = oi.search_by_roi(r, v2, fx.frame_ids[1], Fixture::roi_of(1), {});
    EXPECT_EQ(res.status.code, SearchStatus::Code::ModelMismatch);
    EXPECT_TRUE(res.hits.empty());
    // a different model coexists in its own space; neither sees the other's vectors
    ASSERT_TRUE(oi.index_region(r, v2, fx.frame_ids[1], Fixture::roi_of(1), "y").ok());
    auto a = oi.search_by_roi(r, v1, fx.frame_ids[2], Fixture::roi_of(2), {});
    ASSERT_TRUE(a.status.ok()); ASSERT_EQ(a.hits.size(), 1u); EXPECT_EQ(a.hits[0].region.label, "x");
    // wrong dimension
    auto d = oi.search_by_vector(&r, std::vector<float>(5, 1.f), v1.spec(), {});
    EXPECT_EQ(d.status.code, SearchStatus::Code::DimensionMismatch);
}

TEST(ObjectIndex, MissingJpegHandledGracefully) {
    Fixture fx; fx.build(4);
    std::string e;
    VisualMemoryReader r({fx.dir.str()}); ASSERT_TRUE(r.open(e));
    ObjectIndex oi({fx.dir.str()}); ASSERT_TRUE(oi.open(e));
    MockEmbeddingProvider prov(32);
    ASSERT_TRUE(oi.index_region(r, prov, fx.frame_ids[0], Fixture::roi_of(0), "a").ok());
    ASSERT_TRUE(oi.index_region(r, prov, fx.frame_ids[1], Fixture::roi_of(1), "b").ok());
    FrameDetail fd; ASSERT_TRUE(r.get_frame(fx.frame_ids[0], fd));
    fs::remove(r.absolute_path(fd.record.frame.relative_jpeg_path));                   // JPEG disappears
    ObjectSearchOptions o; o.top_k = 5;
    auto res = oi.search_by_roi(r, prov, fx.frame_ids[2], Fixture::roi_of(2), o);
    ASSERT_TRUE(res.status.ok()); ASSERT_EQ(res.hits.size(), 2u);
    int missing = 0; for (auto& h : res.hits) if (!h.image_exists) { ++missing; EXPECT_EQ(h.region.frame_id, fx.frame_ids[0]); }
    EXPECT_EQ(missing, 1);
    o.require_image = true;
    EXPECT_EQ(oi.search_by_roi(r, prov, fx.frame_ids[2], Fixture::roi_of(2), o).hits.size(), 1u);
    std::vector<uint8_t> jpg;
    EXPECT_EQ(oi.region_crop_jpeg(r, 1, 0, 90, jpg, e), 3);                              // reports unreadable, no crash
    EXPECT_EQ(oi.index_region(r, prov, fx.frame_ids[0], {50, 50, 20, 20}, "").code, ObjectIndexResult::Code::ImageUnavailable);
}

TEST(ObjectIndex, PruneLabelsDeleteAndSchemaGuard) {
    Fixture fx; fx.build(6);
    std::string e;
    VisualMemoryReader r({fx.dir.str()}); ASSERT_TRUE(r.open(e));
    {
        ObjectIndex oi({fx.dir.str()}); ASSERT_TRUE(oi.open(e));
        MockEmbeddingProvider prov(32);
        for (int i = 0; i < 6; ++i) ASSERT_TRUE(oi.index_region(r, prov, fx.frame_ids[i], Fixture::roi_of(i), "").ok());
        EXPECT_TRUE(oi.set_label(1, "mug"));
        EXPECT_FALSE(oi.set_label(1, "bad\nlabel"));
        ObjectFilter f; f.label = "mug"; uint64_t tot = 0;
        EXPECT_EQ(oi.list_regions(f, 10, 0, true, &tot).size(), 1u); EXPECT_EQ(tot, 1u);
        EXPECT_EQ(oi.prune(0, 4), 2u);                         // keep the 4 newest
        EXPECT_EQ(oi.summary().regions, 4u);
        EXPECT_EQ(oi.prune(1, 0), 0u);                         // retention follows index time: nothing is that old
        EXPECT_EQ(oi.prune(std::numeric_limits<int64_t>::max(), 0), 4u);   // everything indexed before "the future" goes
        EXPECT_EQ(oi.summary().regions, 0u);
        for (int i = 0; i < 2; ++i) ASSERT_TRUE(oi.index_region(r, prov, fx.frame_ids[i], Fixture::roi_of(i), "").ok());
        const int64_t last = oi.list_regions({}, 10, 0, true).front().region_id;
        EXPECT_TRUE(oi.delete_region(last));
        EXPECT_FALSE(oi.delete_region(last));
        // vectors follow their region (ON DELETE CASCADE)
        EXPECT_EQ(oi.summary().vector_bytes, oi.summary().regions * 32 * sizeof(float));
    }
    // a database written by a newer library is refused, not modified
    sqlite3* db = nullptr; ASSERT_EQ(sqlite3_open((fs::path(fx.dir.str()) / "objects.db").c_str(), &db), SQLITE_OK);
    sqlite3_exec(db, "PRAGMA user_version=99;", nullptr, nullptr, nullptr); sqlite3_close(db);
    ObjectIndex oi2({fx.dir.str()});
    EXPECT_FALSE(oi2.open(e)); EXPECT_NE(e.find("newer"), std::string::npos);
}

TEST(ObjectIndex, DoesNotTouchFrameDatabase) {
    Fixture fx; fx.build(3);
    std::string e;
    const auto db_path = fs::path(fx.dir.str()) / "visual_memory.db";
    const auto mtime = fs::last_write_time(db_path); const auto size = fs::file_size(db_path);
    VisualMemoryReader r({fx.dir.str()}); ASSERT_TRUE(r.open(e));
    ObjectIndex oi({fx.dir.str()}); ASSERT_TRUE(oi.open(e));
    MockEmbeddingProvider prov(32);
    ASSERT_TRUE(oi.index_region(r, prov, fx.frame_ids[0], Fixture::roi_of(0), "a").ok());
    EXPECT_EQ(fs::last_write_time(db_path), mtime); EXPECT_EQ(fs::file_size(db_path), size);
    EXPECT_TRUE(fs::exists(fs::path(fx.dir.str()) / "objects.db"));
}
