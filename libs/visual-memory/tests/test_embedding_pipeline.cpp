// SPDX-License-Identifier: MIT
// Stage 2 through the public VisualMemory API with the deterministic mock provider:
// pipeline, search semantics, failure handling, retention independence, backfill, restart.
#include "visualmemory/mock_embedding_provider.h"
#include "visualmemory/visual_memory.h"
#include "test_util.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace visualmemory;

namespace {

constexpr int64_t kBase = 1'767'261'600'000LL;   // 2026-01-01T10:00:00Z
constexpr int kW = 64, kH = 64;

Config base_cfg(const std::string& root, bool emb = true) {
    Config c;
    c.root_directory = root;
    c.stored_width = 64; c.stored_height = 64; c.jpeg_quality = 90;
    c.max_pending_frames = 16;
    c.retention_hours = 0;        // tests that need retention set it explicitly
    c.max_storage_bytes = 0;
    c.retention_interval_ms = 40;
    c.embedding.enabled = emb;
    c.embedding.max_pending = 16;
    c.embedding.retention_days = 0;
    c.embedding.backfill_pause_ms = 0;
    return c;
}

ImageView view(const std::vector<uint8_t>& px) {
    ImageView v; v.data = px.data(); v.width = kW; v.height = kH; v.format = PixelFormat::RGB24; return v;
}

bool submit_scene(VisualMemory& vm, uint32_t seed, int64_t ts, uint64_t seq, const std::string& src = "cam", int shift = 0) {
    for (int i = 0; i < 500; ++i) {
        if (vm.try_submit(vmtest::make_scene_frame(kW, kH, seed, src, ts, seq, shift))) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool wait_embedded(VisualMemory& vm, uint64_t n, int ms = 8000) {
    return vmtest::wait_until([&] { return vm.embedding_stats().embedded >= n; }, ms);
}

} // namespace

TEST(EmbeddingPipeline, DisabledByDefaultStage1UnaffectedAndSearchReportsNotEnabled) {
    vmtest::TempDir dir;
    Config c = base_cfg(dir.str(), /*emb=*/false);
    EXPECT_FALSE(Config{}.embedding.enabled);                         // default OFF
    VisualMemory vm(c);
    auto mock = std::make_shared<MockEmbeddingProvider>(16);
    vm.set_embedding_provider(mock);                                  // injected but config disabled
    ASSERT_TRUE(vm.start());
    ASSERT_TRUE(submit_scene(vm, 1, kBase, 1));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == 1; }));
    EXPECT_FALSE(vm.embeddings_active());
    EXPECT_EQ(mock->calls(), 0u);                                     // provider never touched
    SearchStatus st;
    auto r = vm.search_similar(std::vector<float>(16, 1.f), SearchOptions{}, &st);
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(st.code, SearchStatus::Code::NotEnabled);
    EXPECT_EQ(vm.embedding_stats().status, "disabled");
    EXPECT_EQ(vm.query_by_time(0, INT64_MAX, "", 0, 0).size(), 1u);   // Stage 1 query API intact
}

TEST(EmbeddingPipeline, EnabledButNoProviderOrNotReadyDegradesGracefully) {
    {
        vmtest::TempDir dir;
        VisualMemory vm(base_cfg(dir.str()));
        ASSERT_TRUE(vm.start());                                       // no provider: still starts
        EXPECT_FALSE(vm.embeddings_active());
        EXPECT_EQ(vm.embedding_stats().status, "no provider");
        ASSERT_TRUE(submit_scene(vm, 1, kBase, 1));
        EXPECT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == 1; }));
    }
    {
        vmtest::TempDir dir;
        VisualMemory vm(base_cfg(dir.str()));
        auto mock = std::make_shared<MockEmbeddingProvider>(16);
        mock->set_ready(false);                                        // e.g. NPU model failed to load
        vm.set_embedding_provider(mock);
        ASSERT_TRUE(vm.start());
        EXPECT_FALSE(vm.embeddings_active());
        EXPECT_NE(vm.embedding_stats().status.find("not ready"), std::string::npos);
        ASSERT_TRUE(submit_scene(vm, 1, kBase, 1));
        EXPECT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == 1; }));   // Stage 1 unaffected
    }
}

TEST(EmbeddingPipeline, FramesAreEmbeddedAsynchronouslyAndSearchRanksByCosine) {
    vmtest::TempDir dir;
    VisualMemory vm(base_cfg(dir.str()));
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(32));
    ASSERT_TRUE(vm.start());
    ASSERT_TRUE(vm.embeddings_active());
    for (uint32_t i = 0; i < 12; ++i) ASSERT_TRUE(submit_scene(vm, 100 + i, kBase + i * 5000, i + 1));
    ASSERT_TRUE(wait_embedded(vm, 12));
    const auto es = vm.embedding_stats();
    EXPECT_EQ(es.vectors_stored, 12u);
    EXPECT_EQ(es.vector_bytes, 12u * 32 * 4);
    EXPECT_EQ(es.failed_permanent, 0u);
    EXPECT_GT(es.vector_insert.count, 0u);

    // Query: scene 105 slightly brightened -> must retrieve frame #6 (seed 105) first.
    auto q = vmtest::make_scene_rgb(kW, kH, 105, +6);
    SearchOptions o; o.top_k = 5;
    SearchStatus st;
    auto res = vm.search_by_image(view(q), o, &st);
    ASSERT_TRUE(st.ok()) << st.message;
    ASSERT_EQ(res.size(), 5u);
    EXPECT_EQ(res[0].metadata.capture_utc_ms, kBase + 5 * 5000);
    EXPECT_GT(res[0].similarity, 0.95f);
    for (size_t i = 1; i < res.size(); ++i) EXPECT_GE(res[i - 1].similarity, res[i].similarity);   // sorted
    for (auto& r : res) { EXPECT_NEAR(r.distance, 1.0f - r.similarity, 1e-6f); EXPECT_TRUE(r.image_exists);
                          EXPECT_FALSE(r.absolute_path.empty()); EXPECT_GT(r.model_id, 0); EXPECT_EQ(r.metadata.source_id, "cam"); }
    EXPECT_GT(res[0].similarity, res[1].similarity + 0.05f);   // different scenes are distinguishable
    EXPECT_EQ(st.candidates_scanned, 12u);
}

TEST(EmbeddingPipeline, FiltersByTimeSourceThresholdAndTopKClamp) {
    vmtest::TempDir dir;
    VisualMemory vm(base_cfg(dir.str()));
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(32));
    ASSERT_TRUE(vm.start());
    for (uint32_t i = 0; i < 20; ++i) ASSERT_TRUE(submit_scene(vm, 200 + i, kBase + i * 5000, i + 1, i % 2 ? "camB" : "camA"));
    ASSERT_TRUE(wait_embedded(vm, 20));
    auto q = vmtest::make_scene_rgb(kW, kH, 203);
    SearchOptions o; o.top_k = 100;
    auto all = vm.search_by_image(view(q), o);
    EXPECT_EQ(all.size(), 20u);                                       // top_k larger than N is fine
    o.source_id = "camA";
    for (auto& r : vm.search_by_image(view(q), o)) EXPECT_EQ(r.metadata.source_id, "camA");
    o.source_id.clear();
    o.start_utc_ms = kBase + 5 * 5000; o.end_utc_ms = kBase + 10 * 5000;      // [5,10) -> 5 frames
    auto win = vm.search_by_image(view(q), o);
    ASSERT_EQ(win.size(), 5u);
    for (auto& r : win) { EXPECT_GE(r.metadata.capture_utc_ms, kBase + 5 * 5000); EXPECT_LT(r.metadata.capture_utc_ms, kBase + 10 * 5000); }
    o = SearchOptions{}; o.top_k = 100; o.has_min_similarity = true; o.min_similarity = 0.99f;
    auto strict = vm.search_by_image(view(q), o);
    EXPECT_GE(strict.size(), 1u); EXPECT_LT(strict.size(), 20u);      // threshold is opt-in and applied
    for (auto& r : strict) EXPECT_GE(r.similarity, 0.99f);
    o = SearchOptions{}; o.top_k = 0;
    SearchStatus st; vm.search_by_image(view(q), o, &st);
    EXPECT_EQ(st.code, SearchStatus::Code::InvalidArgument);
}

TEST(EmbeddingPipeline, RejectsBadQueriesDimensionNanZeroAndModelMismatch) {
    vmtest::TempDir dir;
    VisualMemory vm(base_cfg(dir.str()));
    auto mock = std::make_shared<MockEmbeddingProvider>(32);
    vm.set_embedding_provider(mock);
    ASSERT_TRUE(vm.start());
    ASSERT_TRUE(submit_scene(vm, 1, kBase, 1));
    ASSERT_TRUE(wait_embedded(vm, 1));
    SearchStatus st;
    vm.search_similar(std::vector<float>(31, 1.f), SearchOptions{}, &st);
    EXPECT_EQ(st.code, SearchStatus::Code::DimensionMismatch);
    std::vector<float> bad(32, 1.f); bad[3] = std::numeric_limits<float>::quiet_NaN();
    vm.search_similar(bad, SearchOptions{}, &st);
    EXPECT_EQ(st.code, SearchStatus::Code::InvalidVector);
    bad[3] = std::numeric_limits<float>::infinity();
    vm.search_similar(bad, SearchOptions{}, &st);
    EXPECT_EQ(st.code, SearchStatus::Code::InvalidVector);
    vm.search_similar(std::vector<float>(32, 0.f), SearchOptions{}, &st);
    EXPECT_EQ(st.code, SearchStatus::Code::InvalidVector);
    EmbeddingSpec other = mock->spec(); other.model_version = "different";
    vm.search_similar(std::vector<float>(32, 1.f), SearchOptions{}, &st, &other);
    EXPECT_EQ(st.code, SearchStatus::Code::ModelMismatch);
    other = mock->spec(); other.preprocessing = "pp2";
    vm.search_similar(std::vector<float>(32, 1.f), SearchOptions{}, &st, &other);
    EXPECT_EQ(st.code, SearchStatus::Code::ModelMismatch);
    EmbeddingSpec same = mock->spec();
    vm.search_similar(std::vector<float>(32, 1.f), SearchOptions{}, &st, &same);
    EXPECT_TRUE(st.ok()) << st.message;
}

TEST(EmbeddingPipeline, ProviderFailuresDoNotAffectStage1AndAreRetriedThenCounted) {
    vmtest::TempDir dir;
    Config c = base_cfg(dir.str()); c.embedding.max_attempts = 2;
    VisualMemory vm(c);
    auto mock = std::make_shared<MockEmbeddingProvider>(16);
    vm.set_embedding_provider(mock);
    ASSERT_TRUE(vm.start());
    mock->fail_next(1);                                               // one transient failure: retry succeeds
    ASSERT_TRUE(submit_scene(vm, 1, kBase, 1));
    ASSERT_TRUE(wait_embedded(vm, 1));
    EXPECT_EQ(vm.embedding_stats().retries, 1u);
    EXPECT_EQ(vm.embedding_stats().inference_failures, 1u);
    mock->fail_next(100);                                             // persistent failure: gives up after max_attempts
    ASSERT_TRUE(submit_scene(vm, 2, kBase + 5000, 2));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.embedding_stats().inference_failures >= 3; }));
    mock->fail_next(0);
    EXPECT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == 2; }));        // Stage 1 persisted both
    EXPECT_EQ(vm.embedding_stats().embedded, 1u);
    EXPECT_EQ(vm.stats().jpeg_write_failures, 0u);
    // the recoverable failure left no DB marker, so backfill embeds it later
    EXPECT_EQ(vm.backfill_embeddings(10), 1u);
    EXPECT_EQ(vm.embedding_stats().embedded, 2u);
}

TEST(EmbeddingPipeline, InvalidVectorsNanAndZeroAreRejectedAndMarkedFailed) {
    vmtest::TempDir dir;
    VisualMemory vm(base_cfg(dir.str()));
    auto mock = std::make_shared<MockEmbeddingProvider>(16);
    vm.set_embedding_provider(mock);
    ASSERT_TRUE(vm.start());
    mock->nan_next(1);
    ASSERT_TRUE(submit_scene(vm, 1, kBase, 1));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.embedding_stats().invalid_vectors >= 1 && vm.embedding_stats().failed_permanent >= 1; }));
    mock->zero_next(1);
    ASSERT_TRUE(submit_scene(vm, 2, kBase + 5000, 2));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.embedding_stats().invalid_vectors >= 2 && vm.embedding_stats().failed_permanent >= 2; }));
    auto es = vm.embedding_stats();
    EXPECT_EQ(es.failed_permanent, 2u);
    EXPECT_EQ(es.embedded, 0u);
    EXPECT_EQ(es.vectors_stored, 0u);
    EXPECT_EQ(vm.backfill_embeddings(10), 0u);                        // permanently failed frames are not retried forever
}

TEST(EmbeddingPipeline, EmbeddingQueueIsBoundedAndDropsAreCountedWithoutBlockingStage1) {
    vmtest::TempDir dir;
    Config c = base_cfg(dir.str()); c.embedding.max_pending = 2; c.max_pending_frames = 64;
    VisualMemory vm(c);
    auto mock = std::make_shared<MockEmbeddingProvider>(16);
    mock->set_delay_ms(200);                                          // slow NPU
    vm.set_embedding_provider(mock);
    ASSERT_TRUE(vm.start());
    const int N = 12;
    for (int i = 0; i < N; ++i) ASSERT_TRUE(submit_scene(vm, 10 + i, kBase + i * 5000, i + 1));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == N; }, 10000));   // Stage 1 never waited for the NPU
    EXPECT_GT(vm.embedding_stats().dropped_queue_full, 0u);
    EXPECT_LE(vm.embedding_stats().max_queue_depth, 2u);
    mock->set_delay_ms(0);
    // dropped frames are recovered by the resumable backfill, exactly once each
    const uint64_t before = vm.embedding_stats().embedded;
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.embedding_stats().queue_depth == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    vm.backfill_embeddings(100);
    EXPECT_EQ(vm.embedding_stats().vectors_stored, static_cast<uint64_t>(N));
    EXPECT_GE(vm.embedding_stats().embedded, before);
    EXPECT_EQ(vm.backfill_embeddings(100), 0u);                       // idempotent: nothing left, no duplicates
}

TEST(EmbeddingPipeline, BackfillEmbedsPreexistingStage1FramesIdempotentlyAndResumes) {
    vmtest::TempDir dir;
    {   // Stage 1 only run (embeddings off)
        VisualMemory vm(base_cfg(dir.str(), false));
        ASSERT_TRUE(vm.start());
        for (uint32_t i = 0; i < 10; ++i) ASSERT_TRUE(submit_scene(vm, 300 + i, kBase + i * 5000, i + 1));
        ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == 10; }));
    }
    VisualMemory vm(base_cfg(dir.str()));
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    ASSERT_TRUE(vm.start());
    EXPECT_EQ(vm.backfill_embeddings(4), 4u);                         // bounded: only 4 this call
    EXPECT_EQ(vm.embedding_stats().vectors_stored, 4u);
    EXPECT_EQ(vm.backfill_embeddings(100), 6u);                       // resumes with the remaining 6
    EXPECT_EQ(vm.backfill_embeddings(100), 0u);                       // idempotent
    EXPECT_EQ(vm.embedding_stats().vectors_stored, 10u);
    EXPECT_EQ(vm.embedding_stats().backfilled, 10u);
    auto q = vmtest::make_scene_rgb(kW, kH, 304);
    auto r = vm.search_by_image(view(q), SearchOptions{});
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r[0].metadata.capture_utc_ms, kBase + 4 * 5000);
}

TEST(EmbeddingPipeline, BackgroundBackfillOnStartupCatchesUp) {
    vmtest::TempDir dir;
    {
        VisualMemory vm(base_cfg(dir.str(), false));
        ASSERT_TRUE(vm.start());
        for (uint32_t i = 0; i < 6; ++i) ASSERT_TRUE(submit_scene(vm, 400 + i, kBase + i * 5000, i + 1));
        ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == 6; }));
    }
    Config c = base_cfg(dir.str()); c.embedding.backfill_on_startup = true; c.embedding.backfill_batch = 2;
    VisualMemory vm(c);
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    ASSERT_TRUE(vm.start());
    EXPECT_TRUE(vmtest::wait_until([&] { return vm.embedding_stats().vectors_stored == 6; }, 10000));
}

TEST(EmbeddingPipeline, EmbeddingsAndSearchSurviveRestart) {
    vmtest::TempDir dir;
    {
        VisualMemory vm(base_cfg(dir.str()));
        vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
        ASSERT_TRUE(vm.start());
        for (uint32_t i = 0; i < 5; ++i) ASSERT_TRUE(submit_scene(vm, 500 + i, kBase + i * 5000, i + 1));
        ASSERT_TRUE(wait_embedded(vm, 5));
    }
    VisualMemory vm(base_cfg(dir.str()));
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    ASSERT_TRUE(vm.start());
    EXPECT_EQ(vm.embedding_stats().vectors_stored, 5u);
    auto q = vmtest::make_scene_rgb(kW, kH, 502);
    auto r = vm.search_by_image(view(q), SearchOptions{});
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r[0].metadata.capture_utc_ms, kBase + 2 * 5000);
    // a new frame after restart is appended with a fresh, non-colliding id
    ASSERT_TRUE(submit_scene(vm, 599, kBase + 99 * 5000, 99));
    ASSERT_TRUE(wait_embedded(vm, 1));
    EXPECT_EQ(vm.embedding_stats().vectors_stored, 6u);
}

TEST(EmbeddingPipeline, ChangingModelVersionNeverMixesSpacesAndRejectsOldQueries) {
    vmtest::TempDir dir;
    {
        VisualMemory vm(base_cfg(dir.str()));
        vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16, "mock-1"));
        ASSERT_TRUE(vm.start());
        ASSERT_TRUE(submit_scene(vm, 1, kBase, 1));
        ASSERT_TRUE(wait_embedded(vm, 1));
    }
    VisualMemory vm(base_cfg(dir.str()));
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16, "mock-2"));   // new model version
    ASSERT_TRUE(vm.start());
    EXPECT_EQ(vm.embedding_stats().vectors_stored, 0u);               // old vectors live in the old space
    auto q = vmtest::make_scene_rgb(kW, kH, 1);
    EXPECT_TRUE(vm.search_by_image(view(q), SearchOptions{}).empty());   // no cross-model comparison
    MockEmbeddingProvider old(16, "mock-1");
    SearchStatus st;
    EmbeddingSpec oldspec = old.spec();
    vm.search_similar(std::vector<float>(16, 1.f), SearchOptions{}, &st, &oldspec);
    EXPECT_EQ(st.code, SearchStatus::Code::ModelMismatch);
    EXPECT_EQ(vm.backfill_embeddings(10), 1u);                        // frame gets a vector in the NEW space
}

TEST(EmbeddingPipeline, MissingJpegIsHandledGracefully) {
    vmtest::TempDir dir;
    VisualMemory vm(base_cfg(dir.str()));
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    ASSERT_TRUE(vm.start());
    for (uint32_t i = 0; i < 3; ++i) ASSERT_TRUE(submit_scene(vm, 600 + i, kBase + i * 5000, i + 1));
    ASSERT_TRUE(wait_embedded(vm, 3));
    auto rows = vm.query_by_time(0, INT64_MAX, "", 0, 0);
    ASSERT_EQ(rows.size(), 3u);
    std::filesystem::remove(vm.absolute_path(rows[1].relative_jpeg_path));      // delete one JPEG behind its back
    auto q = vmtest::make_scene_rgb(kW, kH, 601);
    SearchOptions o; o.top_k = 3;
    auto r = vm.search_by_image(view(q), o);
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].relative_jpeg_path, rows[1].relative_jpeg_path);
    EXPECT_FALSE(r[0].image_exists);                                  // reported, not crashed
    o.require_image = true;
    r = vm.search_by_image(view(q), o);
    EXPECT_EQ(r.size(), 2u);                                          // filtered when the caller needs the image
}

TEST(EmbeddingPipeline, CorruptJpegFailsPermanentlyWithoutHurtingOthers) {
    vmtest::TempDir dir;
    VisualMemory vm(base_cfg(dir.str(), false));          // Stage 1 first
    ASSERT_TRUE(vm.start());
    for (uint32_t i = 0; i < 3; ++i) ASSERT_TRUE(submit_scene(vm, 700 + i, kBase + i * 5000, i + 1));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == 3; }));
    auto rows = vm.query_by_time(0, INT64_MAX, "", 0, 0);
    vm.stop();
    { std::ofstream f(vm.absolute_path(rows[1].relative_jpeg_path), std::ios::binary | std::ios::trunc); f << "not a jpeg at all"; }
    VisualMemory vm2(base_cfg(dir.str()));
    vm2.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    ASSERT_TRUE(vm2.start());
    EXPECT_EQ(vm2.backfill_embeddings(10), 2u);
    EXPECT_EQ(vm2.embedding_stats().decode_failures, 1u);
    EXPECT_EQ(vm2.embedding_stats().failed_permanent, 1u);
    EXPECT_EQ(vm2.backfill_embeddings(10), 0u);                       // not retried every time
}

// ---------- retention: JPEG and embedding lifetimes are independent ----------------------------

TEST(EmbeddingRetention, ExpiredJpegKeepsEmbeddingAndMetadataSearchable) {
    vmtest::TempDir dir;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Config c = base_cfg(dir.str()); c.retention_hours = 1; c.embedding.retention_days = 7;
    VisualMemory vm(c);
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    ASSERT_TRUE(vm.start());
    const int64_t old_ts = now - 3 * 3600 * 1000LL, new_ts = now - 60 * 1000LL;      // 3 h old (JPEG expired), 1 min old
    ASSERT_TRUE(submit_scene(vm, 801, old_ts, 1));
    ASSERT_TRUE(submit_scene(vm, 802, new_ts, 2));
    ASSERT_TRUE(wait_embedded(vm, 2));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().retention_deletions >= 1; }, 8000));
    auto rows = vm.query_by_time(0, INT64_MAX, "", 0, 0);
    ASSERT_EQ(rows.size(), 2u);                                        // metadata row of the old frame still exists
    const FrameInfo* oldf = rows[0].metadata.capture_utc_ms == old_ts ? &rows[0] : &rows[1];
    EXPECT_FALSE(oldf->image_available);
    EXPECT_FALSE(std::filesystem::exists(vm.absolute_path(oldf->relative_jpeg_path)));
    auto q = vmtest::make_scene_rgb(kW, kH, 801);
    SearchOptions o; o.top_k = 2;
    auto r = vm.search_by_image(view(q), o);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].metadata.capture_utc_ms, old_ts);                   // still found by vector search
    EXPECT_FALSE(r[0].image_available);                                // marked unavailable
    EXPECT_FALSE(r[0].image_exists);
    o.require_image = true;
    EXPECT_EQ(vm.search_by_image(view(q), o).size(), 1u);
    EXPECT_EQ(vm.stats().bytes_on_disk, vm.query_by_time(new_ts, new_ts + 1, "", 0, 0)[0].file_size_bytes);   // quota counts only live JPEGs
}

TEST(EmbeddingRetention, EmbeddingExpiresIndependentlyAndOrphanRowIsRemoved) {
    vmtest::TempDir dir;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Config c = base_cfg(dir.str()); c.retention_hours = 1; c.embedding.retention_days = 1;
    VisualMemory vm(c);
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    ASSERT_TRUE(vm.start());
    ASSERT_TRUE(submit_scene(vm, 1, now - 3 * 86400 * 1000LL, 1));   // older than BOTH retentions
    ASSERT_TRUE(submit_scene(vm, 2, now - 60 * 1000LL, 2));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded == 2; }));
    EXPECT_TRUE(vmtest::wait_until([&] { return vm.query_by_time(0, INT64_MAX, "", 0, 0).size() == 1; }, 8000));
    EXPECT_TRUE(vmtest::wait_until([&] { return vm.embedding_stats().vectors_stored == 1; }, 8000));
}

TEST(EmbeddingRetention, Stage1BehaviourUnchangedWhenNoEmbeddingExists) {
    vmtest::TempDir dir;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Config c = base_cfg(dir.str(), false); c.retention_hours = 1;
    VisualMemory vm(c);
    ASSERT_TRUE(vm.start());
    ASSERT_TRUE(submit_scene(vm, 1, now - 3 * 3600 * 1000LL, 1));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().retention_deletions >= 1; }, 8000));
    EXPECT_TRUE(vm.query_by_time(0, INT64_MAX, "", 0, 0).empty());     // row deleted exactly as in Stage 1
}

TEST(EmbeddingRetention, QuotaCoversJpegPlusVectorsAndEvictsOldestJpegFirst) {
    vmtest::TempDir dir;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Config c = base_cfg(dir.str()); c.max_storage_bytes = 15 * 1000; c.embedding.retention_days = 30;
    VisualMemory vm(c);
    vm.set_embedding_provider(std::make_shared<MockEmbeddingProvider>(16));
    ASSERT_TRUE(vm.start());
    for (uint32_t i = 0; i < 30; ++i) ASSERT_TRUE(submit_scene(vm, 900 + i, now - (30 - i) * 1000LL, i + 1));
    ASSERT_TRUE(wait_embedded(vm, 30));
    ASSERT_TRUE(vmtest::wait_until([&] {
        auto s = vm.stats(); auto e = vm.embedding_stats();
        return s.bytes_on_disk + e.vector_bytes <= 15 * 1000 && s.retention_deletions > 0; }, 10000));
    EXPECT_GT(vm.embedding_stats().vectors_stored, 0u);                // vectors outlive the evicted JPEGs
}
