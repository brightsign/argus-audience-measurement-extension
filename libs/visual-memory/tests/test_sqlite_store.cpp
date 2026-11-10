// SPDX-License-Identifier: MIT
// SQLite store: insert/query, source filter, end-exclusive range, ordering,
// pagination, and persistence across reopen.
#include "sqlite_store.h"
#include "test_util.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

using namespace visualmemory;
using namespace visualmemory::detail;

namespace {

InsertRow row(const std::string& src, int64_t cap_ms, uint64_t seq) {
    InsertRow r;
    r.source_id = src;
    r.source_type = "file";
    r.device_id = "dev";
    r.capture_utc_ms = cap_ms;
    r.pts_ns = static_cast<int64_t>(seq) * 1000;
    r.source_sequence = seq;
    r.stored_width = 640;
    r.stored_height = 360;
    r.pixel_format = "BGR24";
    r.relative_jpeg_path = "frames/" + src + "/" + std::to_string(cap_ms) + "_" +
                           std::to_string(seq) + ".jpg";
    r.jpeg_size_bytes = 1000 + seq;
    r.jpeg_sha256 = "deadbeef";
    r.created_utc_ms = cap_ms + 1;
    return r;
}

} // namespace

TEST(SqliteStore, InsertAndQueryByTime) {
    vmtest::TempDir dir;
    SqliteStore db;
    std::string err;
    ASSERT_TRUE(db.open(dir.str() + "/t.db", err)) << err;

    ASSERT_GT(db.insert(row("cam", 1000, 1)), 0);
    ASSERT_GT(db.insert(row("cam", 2000, 2)), 0);
    ASSERT_GT(db.insert(row("cam", 3000, 3)), 0);

    // [1000, 3000) excludes 3000.
    auto r = db.query_by_time(1000, 3000, "", 0, 0);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].metadata.capture_utc_ms, 1000);
    EXPECT_EQ(r[1].metadata.capture_utc_ms, 2000);
    EXPECT_EQ(r[0].metadata.source_sequence, 1u);
}

TEST(SqliteStore, FilterBySource) {
    vmtest::TempDir dir;
    SqliteStore db;
    std::string err;
    ASSERT_TRUE(db.open(dir.str() + "/t.db", err)) << err;
    ASSERT_GT(db.insert(row("camA", 1000, 1)), 0);
    ASSERT_GT(db.insert(row("camB", 1500, 2)), 0);
    ASSERT_GT(db.insert(row("camA", 2000, 3)), 0);

    auto a = db.query_by_time(0, 10000, "camA", 0, 0);
    ASSERT_EQ(a.size(), 2u);
    for (auto& fi : a) EXPECT_EQ(fi.metadata.source_id, "camA");

    auto all = db.query_by_time(0, 10000, "", 0, 0);
    EXPECT_EQ(all.size(), 3u);
}

TEST(SqliteStore, OrderingAndPagination) {
    vmtest::TempDir dir;
    SqliteStore db;
    std::string err;
    ASSERT_TRUE(db.open(dir.str() + "/t.db", err)) << err;
    for (int i = 0; i < 10; ++i)
        ASSERT_GT(db.insert(row("cam", 1000 + i * 100, i + 1)), 0);

    auto page1 = db.query_by_time(0, 1000000, "", 3, 0);
    auto page2 = db.query_by_time(0, 1000000, "", 3, 3);
    ASSERT_EQ(page1.size(), 3u);
    ASSERT_EQ(page2.size(), 3u);
    EXPECT_LT(page1.back().metadata.capture_utc_ms, page2.front().metadata.capture_utc_ms);
    // Sorted ascending within a page.
    EXPECT_LT(page1[0].metadata.capture_utc_ms, page1[1].metadata.capture_utc_ms);
}

TEST(SqliteStore, PersistsAcrossReopen) {
    vmtest::TempDir dir;
    const std::string path = dir.str() + "/t.db";
    {
        SqliteStore db;
        std::string err;
        ASSERT_TRUE(db.open(path, err)) << err;
        ASSERT_GT(db.insert(row("cam", 1000, 1)), 0);
        ASSERT_GT(db.insert(row("cam", 2000, 2)), 0);
    }
    {
        SqliteStore db;
        std::string err;
        ASSERT_TRUE(db.open(path, err)) << err;
        EXPECT_EQ(db.row_count(), 2);
        auto r = db.query_by_time(0, 10000, "", 0, 0);
        ASSERT_EQ(r.size(), 2u);
        EXPECT_EQ(r[0].sha256_hex, "deadbeef");
    }
}

TEST(SqliteStore, TotalBytesAndDelete) {
    vmtest::TempDir dir;
    SqliteStore db;
    std::string err;
    ASSERT_TRUE(db.open(dir.str() + "/t.db", err)) << err;
    int64_t id1 = db.insert(row("cam", 1000, 1));  // 1001 bytes
    int64_t id2 = db.insert(row("cam", 2000, 2));  // 1002 bytes
    ASSERT_GT(id1, 0);
    ASSERT_GT(id2, 0);
    EXPECT_EQ(db.total_jpeg_bytes(), 2003u);

    ASSERT_TRUE(db.delete_by_id(id1));
    EXPECT_EQ(db.row_count(), 1);
    EXPECT_EQ(db.total_jpeg_bytes(), 1002u);
}

// ---- diagnostics of a failed insert (separates a historical transient failure from an active one) ----------------
TEST(SqliteStore, FailedInsertRecordsErrorCodeMessageAndTime) {
    vmtest::TempDir dir; const std::string path = (dir.path() / "visual_memory.db").string();
    SqliteStore s; std::string err; ASSERT_TRUE(s.open(path, err)) << err;
    EXPECT_EQ(s.last_insert_error().count, 0u); EXPECT_EQ(s.last_insert_error().utc_ms, 0);
    ASSERT_GT(s.insert(row("cam", 1000, 1)), 0);
    EXPECT_EQ(s.last_insert_error().count, 0u);                       // success leaves it untouched

    s.set_busy_timeout_ms(50);
    sqlite3* other = nullptr; ASSERT_EQ(sqlite3_open(path.c_str(), &other), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(other, "BEGIN EXCLUSIVE;", nullptr, nullptr, nullptr), SQLITE_OK);     // a second process holds the database
    const int64_t before = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    EXPECT_EQ(s.insert(row("cam", 2000, 77)), -1);
    const DbErrorInfo e = s.last_insert_error();
    EXPECT_EQ(e.count, 1u); EXPECT_EQ(e.code, SQLITE_BUSY); EXPECT_EQ(e.code_name, "SQLITE_BUSY");
    EXPECT_EQ(e.operation, "insert.step"); EXPECT_FALSE(e.message.empty());
    EXPECT_GE(e.utc_ms, before); EXPECT_EQ(e.frame_sequence, 77);
    sqlite3_exec(other, "ROLLBACK;", nullptr, nullptr, nullptr); sqlite3_close(other);

    ASSERT_GT(s.insert(row("cam", 3000, 78)), 0);                      // recovered
    const DbErrorInfo after = s.last_insert_error();                   // the historical record is kept, not erased
    EXPECT_EQ(after.count, 1u); EXPECT_EQ(after.utc_ms, e.utc_ms); EXPECT_EQ(after.frame_sequence, 77);
}

TEST(SqliteStore, InsertOnClosedStoreIsReported) {
    SqliteStore s;
    EXPECT_EQ(s.insert(row("cam", 1000, 5)), -1);
    const DbErrorInfo e = s.last_insert_error();
    EXPECT_EQ(e.count, 1u); EXPECT_EQ(e.code, -1); EXPECT_EQ(e.code_name, "NOT_OPEN"); EXPECT_EQ(e.operation, "insert.not_open");
}
