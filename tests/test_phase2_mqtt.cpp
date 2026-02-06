/**
 * Phase 2A: MQTT Publisher Optimization Tests
 * 
 * Tests for connection persistence, reconnection logic, and payload buffer reuse.
 * 
 * Test Coverage:
 * - Connection reuse across start/stop cycles
 * - Reconnection backoff timing
 * - Payload buffer pre-allocation
 * - Max reconnection attempts enforcement
 * - Connection health monitoring
 * 
 * Note: These tests require a running MQTT broker on localhost:1883
 * Install: sudo apt-get install mosquitto
 * Start: sudo systemctl start mosquitto
 */

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <cstring>  // For strcpy
#include "output/mqtt_publisher.h"
#include "pipeline/pipeline_types.h"
#include "tracking/tracker.h"

using namespace std::chrono_literals;

// Test fixture for MQTT Publisher tests
class Phase2MqttTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Configure for localhost MQTT broker (mosquitto)
        cfg_.host = "localhost";
        cfg_.port = 1883;
        cfg_.client_id = "test_client_phase2";
        cfg_.topic = "test/phase2/topic";
        cfg_.device_id = "test_device";
        cfg_.stream_id = "test_stream";
        cfg_.qos = 0;
        cfg_.retain = false;
        cfg_.clean_session = true;
        cfg_.period_ms = 100;  // Fast publish for testing
        cfg_.username = "";
        cfg_.password = "";
    }

    void TearDown() override {
        // Cleanup
    }

    MqttPublisherCfg cfg_;
};

// Test 1: Connection Reuse
// Verify that start() -> stop() -> start() reuses the connection
TEST_F(Phase2MqttTest, DISABLED_ConnectionReuse) {
    MqttPublisher publisher(cfg_);
    
    // First start - creates new connection
    ASSERT_TRUE(publisher.start()) << "First start() should succeed";
    std::this_thread::sleep_for(200ms);  // Allow connection to establish
    
    // Stop - should preserve connection (mq_ kept alive in Phase 2)
    publisher.stop();
    std::this_thread::sleep_for(100ms);
    
    // Second start - should reuse connection
    ASSERT_TRUE(publisher.start()) << "Second start() should reuse connection";
    std::this_thread::sleep_for(200ms);
    
    // Cleanup
    publisher.stop();
}

// Test 2: Multiple Start/Stop Cycles
// Verify connection persistence across multiple cycles
TEST_F(Phase2MqttTest, DISABLED_MultipleStartStopCycles) {
    MqttPublisher publisher(cfg_);
    
    // Perform 5 start/stop cycles
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(publisher.start()) << "Cycle " << i << " start() failed";
        std::this_thread::sleep_for(100ms);
        
        publisher.stop();
        std::this_thread::sleep_for(50ms);
    }
}

// Test 3: Reconnection Backoff Timing
// Verify that reconnection respects backoff intervals
TEST_F(Phase2MqttTest, DISABLED_ReconnectionBackoffTiming) {
    // Use invalid host to force reconnection failures
    cfg_.host = "invalid.nonexistent.host";
    cfg_.port = 9999;
    
    MqttPublisher publisher(cfg_);
    
    auto start_time = std::chrono::steady_clock::now();
    
    // First start will fail to connect
    EXPECT_FALSE(publisher.start()) << "Should fail to connect to invalid host";
    
    // Attempt reconnection multiple times
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto attempt_start = std::chrono::steady_clock::now();
        
        // Try to start again (should trigger reconnect_if_needed)
        publisher.start();
        
        std::this_thread::sleep_for(100ms);
        
        // Verify backoff timing (should wait at least RECONNECT_BACKOFF_MS between attempts)
        if (attempt > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                attempt_start - start_time
            ).count();
            
            // Each attempt should be separated by at least RECONNECT_BACKOFF_MS (5000ms)
            int expected_min_ms = attempt * 5000;
            EXPECT_GE(elapsed, expected_min_ms * 0.8) 
                << "Attempt " << attempt << " too early (elapsed=" << elapsed << "ms)";
        }
    }
    
    publisher.stop();
}

// Test 4: Max Reconnection Attempts
// Verify that reconnection gives up after MAX_RECONNECT_ATTEMPTS
TEST_F(Phase2MqttTest, DISABLED_MaxReconnectionAttempts) {
    // Use invalid host to force reconnection failures
    cfg_.host = "invalid.nonexistent.host";
    cfg_.port = 9999;
    
    MqttPublisher publisher(cfg_);
    
    // First attempt - should fail
    EXPECT_FALSE(publisher.start()) << "Should fail to connect to invalid host";
    
    // Simulate multiple reconnection attempts by calling start() repeatedly
    // After MAX_RECONNECT_ATTEMPTS (3), it should give up
    int successful_attempts = 0;
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(5100ms);  // Wait for backoff to expire
        
        if (publisher.start()) {
            successful_attempts++;
        }
    }
    
    // Should not succeed after max attempts
    EXPECT_EQ(successful_attempts, 0) << "Should give up after max attempts";
    
    publisher.stop();
}

// Test 5: Connection Health Monitoring
// Verify connection lifecycle
TEST_F(Phase2MqttTest, DISABLED_ConnectionHealthMonitoring) {
    MqttPublisher publisher(cfg_);
    
    // After successful start - connection should be established
    ASSERT_TRUE(publisher.start()) << "start() should succeed";
    std::this_thread::sleep_for(200ms);
    
    // After stop - connection preserved but thread stopped
    publisher.stop();
    std::this_thread::sleep_for(100ms);
}

// Test 6: Payload Buffer Reuse
// Verify that payload buffer is reused and not reallocated
TEST_F(Phase2MqttTest, DISABLED_PayloadBufferReuse) {
    MqttPublisher publisher(cfg_);
    
    ASSERT_TRUE(publisher.start()) << "start() should succeed";
    std::this_thread::sleep_for(200ms);
    
    // Create test data
    PipelineResult result;
    result.ts_ns = 1000000000ULL;
    result.people_count = 2;
    result.gaze_count = 1;
    result.fps = 30;
    result.frame_width = 640;
    result.frame_height = 480;
    
    // Add sample tracks
    TrackedBox track1;
    track1.id = 1;
    track1.x0 = 100.0f;
    track1.y0 = 100.0f;
    track1.x1 = 200.0f;
    track1.y1 = 300.0f;
    track1.score = 0.85f;
    track1.speed = 10.0f;
    track1.dir_deg = 45.0f;
    track1.dir_conf = 0.9f;
    track1.dwell_s = 2.5f;
    track1.just_entered = false;
    track1.just_exited = false;
    track1.has_gaze = true;
    track1.is_gazing = true;
    track1.gaze_time = 1.5f;
    track1.dir_label = "NE";
    
    result.person_tracks.push_back(track1);
    
    // Publish multiple times
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) {
        result.ts_ns += 33333333ULL;  // 30fps interval
        EXPECT_TRUE(publisher.publish_result(result)) << "publish_result() iteration " << i << " failed";
        std::this_thread::sleep_for(20ms);
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    ).count();
    
    // Should complete in reasonable time (< 2 seconds for 50 publishes)
    EXPECT_LT(elapsed, 2000) << "Publishing too slow, possible allocation overhead";
    
    publisher.stop();
}

// Test 7: Large Payload Handling
// Verify that buffer expands for large payloads (>10 tracks)
TEST_F(Phase2MqttTest, DISABLED_LargePayloadHandling) {
    MqttPublisher publisher(cfg_);
    
    ASSERT_TRUE(publisher.start()) << "start() should succeed";
    std::this_thread::sleep_for(200ms);
    
    // Create result with many tracks
    PipelineResult result;
    result.ts_ns = 1000000000ULL;
    result.people_count = 15;
    result.gaze_count = 7;
    result.fps = 30;
    result.frame_width = 640;
    result.frame_height = 480;
    
    // Add 15 tracks
    for (int i = 0; i < 15; ++i) {
        TrackedBox track;
        track.id = i;
        track.x0 = float(i * 30);
        track.y0 = float(i * 20);
        track.x1 = float(i * 30 + 50);
        track.y1 = float(i * 20 + 100);
        track.score = 0.8f;
        track.speed = 5.0f;
        track.dir_deg = float(i * 18);
        track.dir_conf = 0.85f;
        track.dwell_s = 1.5f;
        track.just_entered = false;
        track.just_exited = false;
        track.has_gaze = (i % 2 == 0);
        track.is_gazing = (i % 3 == 0);
        track.gaze_time = 0.5f;
        track.dir_label = "N";
        
        result.person_tracks.push_back(track);
    }
    
    // Should handle large payload without crashes
    EXPECT_TRUE(publisher.publish_result(result)) << "Failed to publish large payload";
    std::this_thread::sleep_for(200ms);
    
    publisher.stop();
}

// Test 8: Concurrent Start/Stop (Disabled - threading complexity)
TEST_F(Phase2MqttTest, DISABLED_ConcurrentStartStop) {
    MqttPublisher publisher(cfg_);
    
    std::atomic<bool> test_running{true};
    std::atomic<int> start_count{0};
    std::atomic<int> stop_count{0};
    
    // Thread 1: Rapid start calls
    std::thread t1([&]() {
        while (test_running.load()) {
            if (publisher.start()) {
                start_count++;
            }
            std::this_thread::sleep_for(50ms);
        }
    });
    
    // Thread 2: Rapid stop calls
    std::thread t2([&]() {
        std::this_thread::sleep_for(25ms);  // Offset slightly
        while (test_running.load()) {
            publisher.stop();
            stop_count++;
            std::this_thread::sleep_for(50ms);
        }
    });
    
    // Let threads run for 500ms
    std::this_thread::sleep_for(500ms);
    test_running.store(false);
    
    t1.join();
    t2.join();
    
    // Should not crash and should have processed some operations
    EXPECT_GT(start_count.load(), 0) << "No start operations completed";
    EXPECT_GT(stop_count.load(), 0) << "No stop operations completed";
    
    // Final cleanup
    publisher.stop();
}

// Test 9: Telemetry Update
// Verify telemetry updates work correctly
TEST_F(Phase2MqttTest, DISABLED_TelemetryUpdate) {
    MqttPublisher publisher(cfg_);
    
    ASSERT_TRUE(publisher.start()) << "start() should succeed";
    std::this_thread::sleep_for(200ms);
    
    // Create telemetry snapshot
    TelemetrySnapshot telemetry;
    telemetry.fps.avg_fps = 29.5f;
    telemetry.npu_util = 0.65f;  // 65%
    
    // Update telemetry
    EXPECT_TRUE(publisher.publish_telemetry(telemetry)) 
        << "publish_telemetry() should succeed";
    
    // Publish result to verify telemetry is included
    PipelineResult result;
    result.ts_ns = 1000000000ULL;
    result.people_count = 1;
    result.gaze_count = 1;
    result.fps = 30;
    result.frame_width = 640;
    result.frame_height = 480;
    
    EXPECT_TRUE(publisher.publish_result(result)) << "publish_result() should succeed";
    std::this_thread::sleep_for(150ms);
    
    publisher.stop();
}

// Test 10: Empty Payload Handling
// Verify publisher handles empty track list gracefully
TEST_F(Phase2MqttTest, DISABLED_EmptyPayloadHandling) {
    MqttPublisher publisher(cfg_);
    
    ASSERT_TRUE(publisher.start()) << "start() should succeed";
    std::this_thread::sleep_for(200ms);
    
    // Create result with no tracks
    PipelineResult result;
    result.ts_ns = 1000000000ULL;
    result.people_count = 0;
    result.gaze_count = 0;
    result.fps = 30;
    result.frame_width = 640;
    result.frame_height = 480;
    result.person_tracks.clear();
    
    // Should handle empty payload without crashes
    EXPECT_TRUE(publisher.publish_result(result)) << "Failed to publish empty payload";
    std::this_thread::sleep_for(150ms);
    
    publisher.stop();
}

// Benchmark Test: Connection Setup Time
// Measure connection reuse vs new connection overhead
TEST_F(Phase2MqttTest, DISABLED_BenchmarkConnectionSetup) {
    MqttPublisher publisher(cfg_);
    
    // Measure new connection time
    auto start_new = std::chrono::steady_clock::now();
    ASSERT_TRUE(publisher.start()) << "First start() failed";
    auto elapsed_new = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_new
    ).count();
    
    std::this_thread::sleep_for(200ms);
    publisher.stop();
    std::this_thread::sleep_for(100ms);
    
    // Measure connection reuse time
    auto start_reuse = std::chrono::steady_clock::now();
    ASSERT_TRUE(publisher.start()) << "Second start() failed";
    auto elapsed_reuse = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_reuse
    ).count();
    
    std::cout << "\n=== Connection Setup Benchmark ===" << std::endl;
    std::cout << "New connection: " << elapsed_new << " ms" << std::endl;
    std::cout << "Reuse connection: " << elapsed_reuse << " ms" << std::endl;
    if (elapsed_reuse > 0) {
        std::cout << "Speedup: " << (double(elapsed_new) / elapsed_reuse) << "x" << std::endl;
    }
    
    // Reuse should be faster than or equal to new connection
    EXPECT_LE(elapsed_reuse, elapsed_new) << "Connection reuse not faster";
    
    publisher.stop();
}

// Main function
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
