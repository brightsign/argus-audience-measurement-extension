// SPDX-License-Identifier: MIT
// End-to-end VisualMemory behaviour: persistence, restart, backpressure,
// validation, graceful failure and clean shutdown.
#include "visualmemory/visual_memory.h"
#include "file_store.h"
#include "sqlite_store.h"
#include "test_util.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <turbojpeg.h>

using namespace visualmemory;

namespace {

Config make_config(const std::string& root) {
    Config c;
    c.root_directory = root;
    c.stored_width = 640;
    c.stored_height = 360;
    c.jpeg_quality = 75;
    c.max_pending_frames = 4;
    c.retention_hours = 100000;            // effectively never for these tests
    c.max_storage_bytes = 0;               // no quota eviction
    c.retention_interval_ms = 1'000'000;   // don't run retention mid-test
    return c;
}

bool is_valid_jpeg(const std::string& path, int& w, int& h) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (bytes.size() < 2) return false;
    tjhandle tj = tjInitDecompress();
    if (!tj) return false;
    int ss = 0, cs = 0;
    int rc = tjDecompressHeader3(tj, bytes.data(), bytes.size(), &w, &h, &ss, &cs);
    tjDestroy(tj);
    return rc == 0;
}

} // namespace

TEST(VisualMemory, PersistsFrameAndRow) {
    vmtest::TempDir dir;
    VisualMemory vm(make_config(dir.str()));
    ASSERT_TRUE(vm.start());

    ASSERT_TRUE(vm.try_submit(vmtest::make_bgr_frame(1280, 720, "cam1", 5000, 1)));
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded >= 1; }));

    auto rows = vm.query_by_time(0, 10'000, "cam1", 0, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].width, 640);
    EXPECT_EQ(rows[0].height, 360);
    EXPECT_EQ(rows[0].metadata.source_id, "cam1");
    EXPECT_FALSE(rows[0].sha256_hex.empty());

    const std::string abs = vm.absolute_path(rows[0].relative_jpeg_path);
    int w = 0, h = 0;
    ASSERT_TRUE(is_valid_jpeg(abs, w, h));
    EXPECT_EQ(w, 640);
    EXPECT_EQ(h, 360);
    vm.stop();
}

TEST(VisualMemory, DataSurvivesRestart) {
    vmtest::TempDir dir;
    {
        VisualMemory vm(make_config(dir.str()));
        ASSERT_TRUE(vm.start());
        ASSERT_TRUE(vm.try_submit(vmtest::make_bgr_frame(640, 480, "cam", 1000, 1)));
        ASSERT_TRUE(vm.try_submit(vmtest::make_bgr_frame(640, 480, "cam", 2000, 2)));
        ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded >= 2; }));
        vm.stop();
    }
    {
        VisualMemory vm(make_config(dir.str()));
        ASSERT_TRUE(vm.start());
        auto rows = vm.query_by_time(0, 10'000, "", 0, 0);
        EXPECT_EQ(rows.size(), 2u);
        vm.stop();
    }
}

TEST(VisualMemory, RejectsInvalidFrame) {
    vmtest::TempDir dir;
    VisualMemory vm(make_config(dir.str()));
    ASSERT_TRUE(vm.start());

    OwnedFrame bad;
    bad.width = 0;             // invalid
    bad.height = 100;
    bad.pixel_format = PixelFormat::BGR24;
    EXPECT_FALSE(vm.try_submit(std::move(bad)));
    EXPECT_EQ(vm.stats().dropped_invalid, 1u);
    vm.stop();
}

TEST(VisualMemory, BackpressureConservesCounters) {
    vmtest::TempDir dir;
    auto cfg = make_config(dir.str());
    cfg.max_pending_frames = 2;
    VisualMemory vm(cfg);
    ASSERT_TRUE(vm.start());

    // Flood faster than the worker can drain to exercise the bounded queue.
    const int N = 200;
    for (int i = 0; i < N; ++i)
        vm.try_submit(vmtest::make_bgr_frame(1280, 720, "cam", 1000 + i, i + 1));

    ASSERT_TRUE(vmtest::wait_until([&] {
        auto s = vm.stats();
        return s.accepted == s.encoded;  // queue drained
    }, 20000));

    auto s = vm.stats();
    EXPECT_EQ(s.samples_seen, static_cast<uint64_t>(N));
    EXPECT_EQ(s.accepted + s.dropped_queue_full + s.dropped_invalid, s.samples_seen);
    EXPECT_GT(s.encoded, 0u);
    vm.stop();
}

TEST(VisualMemory, SubmitAfterStopFails) {
    vmtest::TempDir dir;
    VisualMemory vm(make_config(dir.str()));
    ASSERT_TRUE(vm.start());
    vm.stop();
    EXPECT_FALSE(vm.running());
    EXPECT_FALSE(vm.try_submit(vmtest::make_bgr_frame(64, 64, "cam", 1, 1)));
}

TEST(VisualMemory, UnwritableRootFailsGracefully) {
    // Point the root at a path that cannot be created (a regular file in the way).
    vmtest::TempDir dir;
    const std::string file_as_root = dir.str() + "/not_a_dir";
    std::ofstream(file_as_root) << "x";

    VisualMemory vm(make_config(file_as_root));
    EXPECT_FALSE(vm.start());      // must fail, not crash
    EXPECT_FALSE(vm.running());
    // Submitting while not running simply returns false.
    EXPECT_FALSE(vm.try_submit(vmtest::make_bgr_frame(64, 64, "cam", 1, 1)));
}

TEST(VisualMemory, ShutdownWithPendingDoesNotHang) {
    vmtest::TempDir dir;
    VisualMemory vm(make_config(dir.str()));
    ASSERT_TRUE(vm.start());
    for (int i = 0; i < 50; ++i)
        vm.try_submit(vmtest::make_bgr_frame(1280, 720, "cam", 1000 + i, i + 1));
    vm.stop();  // drain-then-stop: finishes in-flight, discards the rest
    SUCCEED();
}

// Regression: the writer used to run retention to completion before it started draining the ingest queue.
// A store holding a large expired backlog therefore dropped every sample as "queue full" until the pass ended
// (seen on the XT5: ~14,000 expired frames, ~11 min of drops, newest stored frame 2 days old).
TEST(VisualMemory, ExpiredBacklogDoesNotStallIngest) {
    vmtest::TempDir dir;
    const int kBacklog = 600;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    {
        detail::FileStore files(dir.str());
        ASSERT_TRUE(files.init());
        detail::SqliteStore db;
        std::string err;
        ASSERT_TRUE(db.open(dir.str() + "/visual_memory.db", err)) << err;
        for (int i = 0; i < kBacklog; ++i) {
            const int64_t cap = now_ms - 100LL * 3600 * 1000 + i;
            std::string rel = files.make_relative_path("old", cap, i + 1);
            ASSERT_TRUE(files.write_atomic(rel, std::vector<uint8_t>(64, 0xAB)));
            detail::InsertRow r;
            r.source_id = "old"; r.source_type = "file"; r.capture_utc_ms = cap; r.stored_width = 640;
            r.stored_height = 360; r.pixel_format = "BGR24"; r.relative_jpeg_path = rel;
            r.jpeg_size_bytes = 64; r.created_utc_ms = cap;
            ASSERT_GT(db.insert(r), 0);
        }
    }
    auto cfg = make_config(dir.str());
    cfg.retention_hours = 48;
    cfg.retention_slice_deletions = 1;          // many tiny slices: the queue is serviced between each
    cfg.retention_slice_ms = 1;
    VisualMemory vm(cfg);
    ASSERT_TRUE(vm.start());
    for (int i = 0; i < 3; ++i) {
        for (int tries = 0; tries < 2000 && !vm.try_submit(vmtest::make_bgr_frame(640, 480, "cam", now_ms + i, i + 1)); ++tries)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(vmtest::wait_until([&] { return vm.stats().encoded >= 1; }, 10000));
    // The first new frame is committed while the backlog is still being worked off, not after it.
    auto at_first = vm.stats();
    EXPECT_LT(at_first.retention_deletions, static_cast<uint64_t>(kBacklog)) << "ingest waited for the whole backlog";
    EXPECT_GT(at_first.last_commit_utc_ms, 0);

    ASSERT_TRUE(vmtest::wait_until([&] { auto s = vm.stats(); return !s.retention_pending && s.encoded == 3; }, 20000));
    auto s = vm.stats();
    EXPECT_EQ(s.retention_deletions, static_cast<uint64_t>(kBacklog));
    EXPECT_EQ(s.dropped_queue_full, 0u);
    EXPECT_GT(s.retention_slices, 10u);
    EXPECT_GT(s.commit_delay.count, 0u);
    EXPECT_GT(s.retention_slice.count, 0u);
    vm.stop();
}
