#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <string>

#include "demo/runtime_tracker.h"

namespace {

// Unique per-test temp file to avoid collisions when tests run in parallel.
std::string temp_state_file(const std::string& suffix) {
    return "/tmp/argus_test_runtime_" + suffix + ".json";
}

void remove_file(const std::string& path) {
    std::remove(path.c_str());
}

}  // namespace

// --- initialize: new file ---

TEST(RuntimeTracker, InitializeCreatesNewStateFile) {
    const std::string path = temp_state_file("init_new");
    remove_file(path);

    RuntimeTracker tracker(path, 100);
    ASSERT_TRUE(tracker.initialize());

    EXPECT_EQ(tracker.cumulative_seconds(), 0);
    EXPECT_FALSE(tracker.is_expired());
    EXPECT_EQ(tracker.remaining_seconds(), 100);

    remove_file(path);
}

// --- initialize: existing file ---

TEST(RuntimeTracker, InitializeLoadsExistingState) {
    const std::string path = temp_state_file("init_existing");
    remove_file(path);

    // Write initial state via first tracker instance.
    {
        RuntimeTracker t1(path, 200);
        ASSERT_TRUE(t1.initialize());
        t1.update(50);
        t1.flush();
    }

    // Load state in a new instance.
    RuntimeTracker t2(path, 200);
    ASSERT_TRUE(t2.initialize());

    EXPECT_EQ(t2.cumulative_seconds(), 50);
    EXPECT_FALSE(t2.is_expired());
    EXPECT_EQ(t2.remaining_seconds(), 150);

    remove_file(path);
}

// --- expiry detection ---

TEST(RuntimeTracker, DetectsLimitExceeded) {
    const std::string path = temp_state_file("expiry");
    remove_file(path);

    RuntimeTracker tracker(path, 60);
    ASSERT_TRUE(tracker.initialize());

    // One update that exceeds the limit.
    tracker.update(61);

    EXPECT_TRUE(tracker.is_expired());
    EXPECT_EQ(tracker.remaining_seconds(), 0);

    remove_file(path);
}

TEST(RuntimeTracker, NotExpiredWhenBelowLimit) {
    const std::string path = temp_state_file("not_expired");
    remove_file(path);

    RuntimeTracker tracker(path, 1000);
    ASSERT_TRUE(tracker.initialize());
    tracker.update(999);

    EXPECT_FALSE(tracker.is_expired());
    EXPECT_EQ(tracker.remaining_seconds(), 1);

    remove_file(path);
}

// --- corrupted state file ---

TEST(RuntimeTracker, HandlesCorruptedStateFile) {
    const std::string path = temp_state_file("corrupt");

    // Write garbage to simulate corruption.
    {
        std::ofstream ofs(path);
        ofs << "this is not valid json {{{";
    }

    RuntimeTracker tracker(path, 100);
    // Should succeed by starting fresh.
    ASSERT_TRUE(tracker.initialize());

    EXPECT_EQ(tracker.cumulative_seconds(), 0);
    EXPECT_FALSE(tracker.is_expired());

    remove_file(path);
}

// --- persistence across instances ---

TEST(RuntimeTracker, PersistsCumulativeAcrossInstances) {
    const std::string path = temp_state_file("persist");
    remove_file(path);

    // Simulate three separate sessions.
    constexpr int session_seconds = 30;
    for (int session = 0; session < 3; ++session) {
        RuntimeTracker t(path, 1000);
        ASSERT_TRUE(t.initialize());
        t.update(session_seconds);
        t.flush();
    }

    RuntimeTracker final_tracker(path, 1000);
    ASSERT_TRUE(final_tracker.initialize());
    EXPECT_EQ(final_tracker.cumulative_seconds(), 90);

    remove_file(path);
}

// --- update does not accumulate past expiry ---

TEST(RuntimeTracker, UpdateIgnoredAfterExpiry) {
    const std::string path = temp_state_file("update_after_expiry");
    remove_file(path);

    RuntimeTracker tracker(path, 50);
    ASSERT_TRUE(tracker.initialize());

    tracker.update(55);  // Exceeds limit.
    ASSERT_TRUE(tracker.is_expired());

    int seconds_at_expiry = tracker.cumulative_seconds();
    tracker.update(100);  // Should be ignored.
    EXPECT_EQ(tracker.cumulative_seconds(), seconds_at_expiry);

    remove_file(path);
}

// --- flush writes to disk ---

TEST(RuntimeTracker, FlushWritesStateToDisk) {
    const std::string path = temp_state_file("flush");
    remove_file(path);

    {
        RuntimeTracker tracker(path, 500);
        ASSERT_TRUE(tracker.initialize());
        tracker.update(42);
        tracker.flush();
    }

    // Read back via a new instance without calling update.
    RuntimeTracker reader(path, 500);
    ASSERT_TRUE(reader.initialize());
    EXPECT_EQ(reader.cumulative_seconds(), 42);

    remove_file(path);
}

// --- first_started_utc is set only once ---

TEST(RuntimeTracker, FirstStartedUtcPreservedAcrossSessions) {
    const std::string path = temp_state_file("first_started");
    remove_file(path);

    std::string first_ts;
    {
        RuntimeTracker t1(path, 200);
        ASSERT_TRUE(t1.initialize());
        first_ts = t1.first_started_utc();
        EXPECT_FALSE(first_ts.empty());
        t1.flush();
    }

    {
        RuntimeTracker t2(path, 200);
        ASSERT_TRUE(t2.initialize());
        EXPECT_EQ(t2.first_started_utc(), first_ts);
    }

    remove_file(path);
}
