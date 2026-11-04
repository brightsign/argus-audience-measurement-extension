// SPDX-License-Identifier: MIT
// VisualMemoryReader: read-only second-process access (dashboard API foundation).
#include "visualmemory/mock_embedding_provider.h"
#include "visualmemory/reader.h"
#include "visualmemory/visual_memory.h"
#include "test_util.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>

using namespace visualmemory;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kBase = 1'767'261'600'000LL;
constexpr int kW = 64, kH = 64;

Config cfg(const std::string& root) {
    Config c;
    c.root_directory = root;
    c.stored_width = 64; c.stored_height = 64; c.jpeg_quality = 90;
    c.max_pending_frames = 16; c.retention_hours = 0; c.max_storage_bytes = 0;
    c.embedding.enabled = true; c.embedding.max_pending = 16; c.embedding.retention_days = 0;
    c.embedding.backfill_pause_ms = 0;
    return c;
}

bool submit(VisualMemory& vm, uint32_t seed, int64_t ts, uint64_t seq, const std::string& src = "cam") {
    for (int i = 0; i < 500; ++i) {
        if (vm.try_submit(vmtest::make_scene_frame(kW, kH, seed, src, ts, seq, 0))) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Writer process stand-in: N distinct scenes, 5 s apart, embedded with the mock provider.
void populate(const std::string& root, int n, const std::string& src = "cam", bool embed = true) {
    Config c = cfg(root); c.embedding.enabled = embed;
    VisualMemory vm(c);
    if (embed) vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    ASSERT_TRUE(vm.start());
    for (int i = 0; i < n; ++i) ASSERT_TRUE(submit(vm, 100 + i, kBase + i * 5000, i + 1, src));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == static_cast<uint64_t>(n); }));
    if (embed) ASSERT_TRUE(vmtest::wait_until([&] { return vm.embedding_stats().embedded == static_cast<uint64_t>(n); }));
    vm.stop();
}

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
} // namespace

TEST(Reader, OpenFailsCleanlyWhenMissingOrNewerSchemaRequired) {
    vmtest::TempDir dir;
    VisualMemoryReader r({dir.str()});
    std::string err;
    EXPECT_FALSE(r.open(err));
    EXPECT_NE(err.find("not found"), std::string::npos);
    // A v1 (Stage 1) database must be reported, never migrated by the reader.
    fs::create_directories(dir.str());
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open((fs::path(dir.str()) / "visual_memory.db").c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE frames(id INTEGER PRIMARY KEY); PRAGMA user_version=1;", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
    err.clear();
    EXPECT_FALSE(r.open(err));
    EXPECT_NE(err.find("schema"), std::string::npos);
}

TEST(Reader, ListGetAndSummaryReflectPersistedData) {
    vmtest::TempDir dir;
    populate(dir.str(), 12);
    VisualMemoryReader r({dir.str()});
    std::string err;
    ASSERT_TRUE(r.open(err)) << err;
    auto s = r.summary();
    EXPECT_TRUE(s.open);
    EXPECT_EQ(s.schema_version, 2);
    EXPECT_EQ(s.frames, 12u);
    EXPECT_EQ(s.frames_with_image, 12u);
    EXPECT_EQ(s.embeddings_ok, 12u);
    EXPECT_EQ(s.vector_bytes, 12u * 16u * 4u);
    EXPECT_GT(s.jpeg_bytes, 0u);
    EXPECT_EQ(s.oldest_capture_utc_ms, kBase);
    EXPECT_EQ(s.newest_capture_utc_ms, kBase + 11 * 5000);
    ASSERT_EQ(s.sources.size(), 1u);
    ASSERT_EQ(s.models.size(), 1u);

    uint64_t total = 0;
    auto page = r.list_frames({}, 5, 0, true, &total);
    EXPECT_EQ(total, 12u);
    ASSERT_EQ(page.size(), 5u);
    EXPECT_EQ(page[0].frame.metadata.capture_utc_ms, kBase + 11 * 5000);        // newest first
    EXPECT_TRUE(page[0].image_exists);
    EXPECT_TRUE(page[0].has_embedding);
    auto page2 = r.list_frames({}, 5, 10, true, nullptr);
    EXPECT_EQ(page2.size(), 2u);
    FrameFilter f; f.start_utc_ms = kBase + 5000; f.end_utc_ms = kBase + 5 * 5000;
    r.list_frames(f, 50, 0, false, &total);
    EXPECT_EQ(total, 4u);                                                       // [start,end)
    f = {}; f.source_id = "nope";
    r.list_frames(f, 50, 0, false, &total);
    EXPECT_EQ(total, 0u);

    FrameDetail d;
    ASSERT_TRUE(r.get_frame(page[0].frame.id, d));
    EXPECT_EQ(d.record.frame.sha256_hex.size(), 64u);
    ASSERT_EQ(d.embeddings.size(), 1u);
    EXPECT_TRUE(d.embeddings[0].ok);
    EXPECT_EQ(d.embeddings[0].dimension, 16);
    EXPECT_FALSE(r.get_frame(999999, d));
}

TEST(Reader, ReadJpegHandlesUnknownExpiredAndMissing) {
    vmtest::TempDir dir;
    populate(dir.str(), 3);
    VisualMemoryReader r({dir.str()});
    std::string err;
    ASSERT_TRUE(r.open(err)) << err;
    auto page = r.list_frames({}, 10, 0, false);
    ASSERT_EQ(page.size(), 3u);
    std::vector<uint8_t> jpg;
    EXPECT_EQ(r.read_jpeg(page[0].frame.id, jpg, err), 0);
    ASSERT_GT(jpg.size(), 4u);
    EXPECT_EQ(jpg[0], 0xFF); EXPECT_EQ(jpg[1], 0xD8);
    EXPECT_EQ(r.read_jpeg(424242, jpg, err), 1);
    // delete the file behind the DB's back -> "missing"
    fs::remove(r.absolute_path(page[1].frame.relative_jpeg_path));
    EXPECT_EQ(r.read_jpeg(page[1].frame.id, jpg, err), 3);
    auto again = r.list_frames({}, 10, 0, false);
    EXPECT_TRUE(again[1].frame.image_available);
    EXPECT_FALSE(again[1].image_exists);
}

TEST(Reader, ExpiredJpegKeepsRowAndEmbeddingAndReportsExpired) {
    vmtest::TempDir dir;
    {
        Config c = cfg(dir.str());
        c.retention_hours = 1; c.retention_interval_ms = 40;
        c.embedding.retention_days = 3650;
        VisualMemory vm(c);
        vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
        ASSERT_TRUE(vm.start());
        ASSERT_TRUE(submit(vm, 1, kBase, 1));                       // years old -> JPEG expires at once
        ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == 1; }));
        ASSERT_TRUE(vmtest::wait_until([&] { return vm.embedding_stats().embedded == 1; }));
        ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().retention_deletions >= 1; }));
        vm.stop();
    }
    VisualMemoryReader r({dir.str()});
    std::string err;
    ASSERT_TRUE(r.open(err)) << err;
    auto page = r.list_frames({}, 10, 0, false);
    ASSERT_EQ(page.size(), 1u);
    EXPECT_FALSE(page[0].frame.image_available);
    EXPECT_FALSE(page[0].image_exists);
    EXPECT_TRUE(page[0].has_embedding);
    std::vector<uint8_t> jpg;
    EXPECT_EQ(r.read_jpeg(page[0].frame.id, jpg, err), 2);
}

TEST(Reader, SearchByJpegAndByFrameNeedMatchingProviderAndExcludeSelf) {
    vmtest::TempDir dir;
    populate(dir.str(), 10);
    VisualMemoryReader r({dir.str()});
    std::string err;
    ASSERT_TRUE(r.open(err)) << err;
    SearchOptions o; o.top_k = 3;
    SearchStatus st;

    // no provider: uploads are refused, frame search still works from the stored vector
    auto none = r.search_by_jpeg({0xFF, 0xD8}, o, &st);
    EXPECT_TRUE(none.empty());
    EXPECT_NE(st.code, SearchStatus::Code::Ok);

    auto page = r.list_frames({}, 10, 0, false);
    const int64_t qid = page[4].frame.id;
    bool stored = false;
    auto res = r.search_by_frame_id(qid, o, &st, &stored);
    ASSERT_TRUE(st.ok()) << st.message;
    EXPECT_TRUE(stored);
    ASSERT_EQ(res.size(), 3u);
    for (auto& h : res) EXPECT_NE(h.frame_id, qid);                 // query frame excluded
    EXPECT_GE(res[0].similarity, res[1].similarity);

    // with the (mock) provider, an upload of frame #4's own pixels ranks that frame first (similarity ~1)
    r.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    std::vector<uint8_t> jpg; ASSERT_EQ(r.read_jpeg(qid, jpg, err), 0);
    auto up = r.search_by_jpeg(jpg, o, &st);
    ASSERT_TRUE(st.ok()) << st.message;
    ASSERT_FALSE(up.empty());
    EXPECT_EQ(up[0].frame_id, qid);
    EXPECT_GT(up[0].similarity, 0.99f);

    // a provider of a different space is rejected, not silently compared
    r.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(32));
    auto bad = r.search_by_jpeg(jpg, o, &st);
    EXPECT_TRUE(bad.empty());
    EXPECT_EQ(st.code, SearchStatus::Code::ModelMismatch);

    // unknown frame, corrupt upload
    r.search_by_frame_id(31337, o, &st);
    EXPECT_EQ(st.code, SearchStatus::Code::InvalidArgument);
    r.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    r.search_by_jpeg({1, 2, 3, 4}, o, &st);
    EXPECT_EQ(st.code, SearchStatus::Code::InvalidArgument);
}

TEST(Reader, FrameSearchOnStage1OnlyStoreReportsNotEnabled) {
    vmtest::TempDir dir;
    populate(dir.str(), 3, "cam", /*embed=*/false);
    VisualMemoryReader r({dir.str()});
    std::string err;
    ASSERT_TRUE(r.open(err)) << err;
    EXPECT_EQ(r.summary().embeddings_ok, 0u);
    SearchStatus st;
    auto page = r.list_frames({}, 10, 0, false);
    r.search_by_frame_id(page[0].frame.id, {}, &st);
    EXPECT_EQ(st.code, SearchStatus::Code::NotEnabled);
    EXPECT_FALSE(page[0].has_embedding);
}

TEST(Reader, NeverModifiesTheStoreAndCoexistsWithARunningWriter) {
    vmtest::TempDir dir;
    populate(dir.str(), 6);
    const auto dbp = fs::path(dir.str()) / "visual_memory.db";
    const std::string before = slurp(dbp);
    {
        VisualMemoryReader r({dir.str()});
        std::string err;
        ASSERT_TRUE(r.open(err)) << err;
        r.summary(); r.list_frames({}, 10, 0, true);
        SearchStatus st; r.search_by_frame_id(r.list_frames({}, 1, 0, true)[0].frame.id, {}, &st);
        EXPECT_TRUE(st.ok());
    }
    EXPECT_EQ(slurp(dbp), before);                                   // byte-identical, no journal left
    EXPECT_FALSE(fs::exists(fs::path(dir.str()) / "visual_memory.db-journal"));

    // reader open while the writer is live and inserting
    Config c = cfg(dir.str());
    VisualMemory vm(c);
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    ASSERT_TRUE(vm.start());
    VisualMemoryReader r({dir.str()});
    std::string err;
    ASSERT_TRUE(r.open(err)) << err;
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(submit(vm, 500 + i, kBase + 100000 + i * 5000, 100 + i));
    for (int i = 0; i < 20; ++i) { r.summary(); r.list_frames({}, 5, 0, true); }
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == 5; }));
    EXPECT_EQ(r.summary().frames, 11u);
    vm.stop();
}

TEST(Reader, CancelSearchIsSafeWhenIdleAndAfterwardsSearchStillWorks) {
    vmtest::TempDir dir;
    populate(dir.str(), 4);
    VisualMemoryReader r({dir.str()});
    std::string err;
    ASSERT_TRUE(r.open(err)) << err;
    r.cancel_search();                                               // nothing running: no effect
    SearchStatus st;
    auto id = r.list_frames({}, 1, 0, true)[0].frame.id;
    r.search_by_frame_id(id, {}, &st);
    EXPECT_TRUE(st.ok()) << st.message;
}

TEST(Reader, ThumbnailIsValidSmallerJpegAndHandlesExpired) {
    vmtest::TempDir dir;
    populate(dir.str(), 2);
    VisualMemoryReader r({dir.str()});
    std::string err;
    ASSERT_TRUE(r.open(err)) << err;
    auto id = r.list_frames({}, 1, 0, true)[0].frame.id;
    std::vector<uint8_t> t, full;
    ASSERT_EQ(r.read_thumbnail(id, 32, t, err), 0) << err;
    ASSERT_EQ(r.read_jpeg(id, full, err), 0);
    ASSERT_GT(t.size(), 4u);
    EXPECT_EQ(t[0], 0xFF); EXPECT_EQ(t[1], 0xD8);
    EXPECT_EQ(r.read_thumbnail(777777, 32, t, err), 1);
    // never echoes filesystem paths
    fs::remove(r.absolute_path(r.list_frames({}, 1, 0, true)[0].frame.relative_jpeg_path));
    EXPECT_EQ(r.read_thumbnail(id, 32, t, err), 3);
    EXPECT_EQ(err.find(dir.str()), std::string::npos);
}
