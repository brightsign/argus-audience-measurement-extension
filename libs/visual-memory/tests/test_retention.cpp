// SPDX-License-Identifier: MIT
// Retention: age expiry, quota eviction, missing-file tolerance, temp reap.
#include "file_store.h"
#include "retention.h"
#include "sqlite_store.h"
#include "test_util.h"

#include <fstream>
#include <gtest/gtest.h>

using namespace visualmemory;
using namespace visualmemory::detail;

namespace {

// Insert a DB row AND create its JPEG file so both can be evicted together.
int64_t add_frame(SqliteStore& db, FileStore& files, const std::string& src,
                  int64_t cap_ms, uint64_t seq, uint64_t size_bytes) {
    std::string rel = files.make_relative_path(src, cap_ms, seq);
    std::vector<uint8_t> bytes(size_bytes, 0xAB);
    EXPECT_TRUE(files.write_atomic(rel, bytes));

    InsertRow r;
    r.source_id = src;
    r.source_type = "file";
    r.device_id = "dev";
    r.capture_utc_ms = cap_ms;
    r.stored_width = 640;
    r.stored_height = 360;
    r.pixel_format = "BGR24";
    r.relative_jpeg_path = rel;
    r.jpeg_size_bytes = size_bytes;
    r.created_utc_ms = cap_ms;
    return db.insert(r);
}

} // namespace

TEST(Retention, AgeExpiry) {
    vmtest::TempDir dir;
    FileStore files(dir.str());
    ASSERT_TRUE(files.init());
    SqliteStore db;
    std::string err;
    ASSERT_TRUE(db.open(dir.str() + "/t.db", err)) << err;

    const int64_t now = 100LL * 3600 * 1000;  // 100h in ms
    add_frame(db, files, "cam", now - 40LL * 3600 * 1000, 1, 1000);  // 40h old -> keep
    add_frame(db, files, "cam", now - 10LL * 3600 * 1000, 2, 1000);  // 10h old -> keep
    add_frame(db, files, "cam", now - 60LL * 3600 * 1000, 3, 1000);  // 60h old -> expire

    auto res = run_retention(db, files, now, /*retention_hours=*/48,
                             /*max_storage_bytes=*/0);
    EXPECT_EQ(res.deleted, 1u);
    EXPECT_EQ(db.row_count(), 2);
}

TEST(Retention, BudgetedSlicesMakeProgressAndFinish) {
    // A large expired backlog must be removable in bounded slices (the writer thread interleaves the ingest
    // queue between slices); the sum of the slices equals one unbounded pass.
    vmtest::TempDir dir;
    FileStore files(dir.str());
    ASSERT_TRUE(files.init());
    SqliteStore db;
    std::string err;
    ASSERT_TRUE(db.open(dir.str() + "/t.db", err)) << err;
    const int64_t now = 1000LL * 3600 * 1000;
    for (int i = 0; i < 300; ++i) add_frame(db, files, "cam", now - 100LL * 3600 * 1000 + i, i + 1, 200);
    add_frame(db, files, "cam", now - 1000, 999, 200);      // fresh: must survive

    RetentionBudget b; b.max_deletions = 40;
    uint64_t total = 0; int slices = 0;
    for (;;) {
        auto r = run_retention(db, files, now, 48, 0, nullptr, 0, b);
        EXPECT_LE(r.deleted, 40u);
        total += r.deleted; ++slices;
        if (!r.more) break;
        ASSERT_LT(slices, 100) << "slices never converged";
    }
    EXPECT_EQ(total, 300u);
    EXPECT_EQ(slices, 8);                     // 7 full slices of 40 + the remainder (20)
    EXPECT_EQ(db.row_count(), 1);
}

TEST(Retention, TimeBudgetStopsEarly) {
    vmtest::TempDir dir;
    FileStore files(dir.str());
    ASSERT_TRUE(files.init());
    SqliteStore db;
    std::string err;
    ASSERT_TRUE(db.open(dir.str() + "/t.db", err)) << err;
    const int64_t now = 1000LL * 3600 * 1000;
    for (int i = 0; i < 50; ++i) add_frame(db, files, "cam", now - 100LL * 3600 * 1000 + i, i + 1, 200);
    RetentionBudget b; b.max_ms = 1; b.max_deletions = 0;
    int slices = 0; uint64_t total = 0;
    for (;;) {
        auto r = run_retention(db, files, now, 48, 0, nullptr, 0, b);
        total += r.deleted; ++slices;
        if (!r.more) break;
        ASSERT_LT(slices, 100000);
    }
    EXPECT_EQ(total, 50u);                    // however many slices it took, nothing is lost or double counted
    EXPECT_EQ(db.row_count(), 0);
}

TEST(Retention, QuotaEviction) {
    vmtest::TempDir dir;
    FileStore files(dir.str());
    ASSERT_TRUE(files.init());
    SqliteStore db;
    std::string err;
    ASSERT_TRUE(db.open(dir.str() + "/t.db", err)) << err;

    // 5 frames x 1000 bytes = 5000; quota 2500 -> evict oldest until <=2500.
    for (int i = 0; i < 5; ++i)
        add_frame(db, files, "cam", 1000 + i * 100, i + 1, 1000);

    auto res = run_retention(db, files, 1'000'000, /*retention_hours=*/0,
                             /*max_storage_bytes=*/2500);
    EXPECT_GE(res.deleted, 3u);
    EXPECT_LE(db.total_jpeg_bytes(), 2500u);

    // Newest frames survive (oldest evicted first).
    auto remaining = db.query_by_time(0, 10'000, "", 0, 0);
    ASSERT_FALSE(remaining.empty());
    EXPECT_GE(remaining.front().metadata.capture_utc_ms, 1300);
}

TEST(Retention, ToleratesMissingFile) {
    vmtest::TempDir dir;
    FileStore files(dir.str());
    ASSERT_TRUE(files.init());
    SqliteStore db;
    std::string err;
    ASSERT_TRUE(db.open(dir.str() + "/t.db", err)) << err;

    const int64_t now = 100LL * 3600 * 1000;
    add_frame(db, files, "cam", now - 60LL * 3600 * 1000, 1, 1000);
    // Delete the file out from under the DB; retention must still remove the row.
    auto rows = db.query_by_time(0, now, "", 0, 0);
    ASSERT_EQ(rows.size(), 1u);
    std::filesystem::remove(files.absolute(rows[0].relative_jpeg_path));

    auto res = run_retention(db, files, now, 48, 0);
    EXPECT_EQ(res.deleted, 1u);
    EXPECT_EQ(db.row_count(), 0);
}

TEST(FileStore, ReapsTempFiles) {
    vmtest::TempDir dir;
    FileStore files(dir.str());
    ASSERT_TRUE(files.init());
    // Simulate an interrupted write.
    auto tmp = std::filesystem::path(dir.str()) / "frames" / "2026" / "01" / "01";
    std::filesystem::create_directories(tmp);
    std::ofstream(tmp / "orphan.jpg.tmp") << "partial";
    EXPECT_EQ(files.reap_temp_files(), 1);
    EXPECT_FALSE(std::filesystem::exists(tmp / "orphan.jpg.tmp"));
}

TEST(FileStore, RejectsTraversalPaths) {
    vmtest::TempDir dir;
    FileStore files(dir.str());
    ASSERT_TRUE(files.init());
    EXPECT_FALSE(files.is_safe_relative("../escape.jpg"));
    EXPECT_FALSE(files.is_safe_relative("/absolute.jpg"));
    EXPECT_FALSE(files.is_safe_relative("frames/../../etc/passwd"));
    EXPECT_TRUE(files.is_safe_relative("frames/2026/01/01/cam/x.jpg"));
    std::vector<uint8_t> b{1, 2, 3};
    EXPECT_FALSE(files.write_atomic("../evil.jpg", b));
}
