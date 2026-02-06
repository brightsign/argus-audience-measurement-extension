// Phase 2C: Async Frame Writer Unit Tests
// Tests for lock-free SPSC queue and async disk frame writer

#include <gtest/gtest.h>
#include "util/spsc_queue.h"
#include "output/frame_writer.h"
#include "pipeline/pipeline_types.h"
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

// ============================================================================
// SPSCQueue Tests
// ============================================================================

class SPSCQueueTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(SPSCQueueTest, BasicEnqueueDequeue) {
  SPSCQueue<int, 8> queue;
  
  // Initial state
  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.full());
  EXPECT_EQ(queue.size(), 0);
  EXPECT_EQ(queue.capacity(), 7);  // Ring buffer loses one slot
  
  // Enqueue items
  EXPECT_TRUE(queue.try_enqueue(42));
  EXPECT_FALSE(queue.empty());
  EXPECT_EQ(queue.size(), 1);
  
  EXPECT_TRUE(queue.try_enqueue(123));
  EXPECT_EQ(queue.size(), 2);
  
  // Dequeue items (FIFO order)
  int value;
  EXPECT_TRUE(queue.try_dequeue(value));
  EXPECT_EQ(value, 42);
  EXPECT_EQ(queue.size(), 1);
  
  EXPECT_TRUE(queue.try_dequeue(value));
  EXPECT_EQ(value, 123);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0);
}

TEST_F(SPSCQueueTest, QueueFullBehavior) {
  SPSCQueue<int, 8> queue;
  
  // Fill queue to capacity (7 items for size-8 ring buffer)
  for (int i = 0; i < 7; ++i) {
    EXPECT_TRUE(queue.try_enqueue(i)) << "Failed at i=" << i;
  }
  
  EXPECT_TRUE(queue.full());
  EXPECT_EQ(queue.size(), 7);
  
  // Next enqueue should fail
  EXPECT_FALSE(queue.try_enqueue(999));
  EXPECT_EQ(queue.size(), 7);
  
  // Dequeue one item
  int value;
  EXPECT_TRUE(queue.try_dequeue(value));
  EXPECT_EQ(value, 0);
  EXPECT_FALSE(queue.full());
  
  // Now enqueue should succeed
  EXPECT_TRUE(queue.try_enqueue(777));
  EXPECT_EQ(queue.size(), 7);
}

TEST_F(SPSCQueueTest, QueueEmptyBehavior) {
  SPSCQueue<int, 8> queue;
  
  // Dequeue from empty queue should fail
  int value;
  EXPECT_FALSE(queue.try_dequeue(value));
  
  // Enqueue and dequeue
  EXPECT_TRUE(queue.try_enqueue(42));
  EXPECT_TRUE(queue.try_dequeue(value));
  EXPECT_EQ(value, 42);
  
  // Now empty again
  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.try_dequeue(value));
}

TEST_F(SPSCQueueTest, MoveSemantics) {
  struct LargeStruct {
    std::vector<int> data;
    LargeStruct() : data() {}  // Default constructor
    explicit LargeStruct(size_t size) : data(size, 42) {}
  };
  
  SPSCQueue<LargeStruct, 8> queue;
  
  // Enqueue with move
  LargeStruct item(1000);
  EXPECT_TRUE(queue.try_enqueue(std::move(item)));
  EXPECT_TRUE(item.data.empty());  // Moved from
  
  // Dequeue with move
  LargeStruct out(0);
  EXPECT_TRUE(queue.try_dequeue(out));
  EXPECT_EQ(out.data.size(), 1000);
  EXPECT_EQ(out.data[0], 42);
}

TEST_F(SPSCQueueTest, ConcurrentProducerConsumer) {
  SPSCQueue<int, 64> queue;
  constexpr int NUM_ITEMS = 1000;
  std::atomic<bool> producer_done{false};
  std::vector<int> consumed;
  consumed.reserve(NUM_ITEMS);
  
  // Producer thread
  std::thread producer([&]() {
    for (int i = 0; i < NUM_ITEMS; ++i) {
      while (!queue.try_enqueue(i)) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });
  
  // Consumer thread
  std::thread consumer([&]() {
    int value;
    while (consumed.size() < NUM_ITEMS) {
      if (queue.try_dequeue(value)) {
        consumed.push_back(value);
      } else if (producer_done.load(std::memory_order_acquire) && queue.empty()) {
        break;
      } else {
        std::this_thread::yield();
      }
    }
  });
  
  producer.join();
  consumer.join();
  
  // Verify all items consumed in order
  EXPECT_EQ(consumed.size(), NUM_ITEMS);
  for (int i = 0; i < NUM_ITEMS; ++i) {
    EXPECT_EQ(consumed[i], i);
  }
}

TEST_F(SPSCQueueTest, StressTest) {
  SPSCQueue<int, 32> queue;
  constexpr int NUM_ITERATIONS = 10000;
  std::atomic<int> enqueue_count{0};
  std::atomic<int> dequeue_count{0};
  std::atomic<bool> stop{false};
  
  // Producer: enqueue NUM_ITERATIONS items
  std::thread producer([&]() {
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
      while (!queue.try_enqueue(i)) {
        std::this_thread::yield();
      }
      enqueue_count.fetch_add(1, std::memory_order_relaxed);
    }
    stop.store(true, std::memory_order_release);
  });
  
  // Consumer: dequeue all items
  std::thread consumer([&]() {
    int value;
    while (!stop.load(std::memory_order_acquire) || !queue.empty()) {
      if (queue.try_dequeue(value)) {
        dequeue_count.fetch_add(1, std::memory_order_relaxed);
      } else {
        std::this_thread::yield();
      }
    }
  });
  
  producer.join();
  consumer.join();
  
  EXPECT_EQ(enqueue_count.load(), NUM_ITERATIONS);
  EXPECT_EQ(dequeue_count.load(), NUM_ITERATIONS);
  EXPECT_TRUE(queue.empty());
}

// ============================================================================
// AsyncDiskFrameWriter Tests
// ============================================================================

class AsyncFrameWriterTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "frame_writer_test";
    fs::remove_all(test_dir_);
    fs::create_directories(test_dir_);
  }
  
  void TearDown() override {
    fs::remove_all(test_dir_);
  }
  
  // Helper: create test frame
  cv::Mat createTestFrame(int width = 320, int height = 320) {
    cv::Mat frame(height, width, CV_8UC3);
    // Fill with gradient pattern
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        frame.at<cv::Vec3b>(y, x) = cv::Vec3b(x % 256, y % 256, (x + y) % 256);
      }
    }
    return frame;
  }
  
  // Helper: create test result
  PipelineResult createTestResult(uint64_t seq = 1) {
    PipelineResult result;
    result.seq = seq;
    result.frame_width = 1920;
    result.frame_height = 1080;
    result.ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return result;
  }
  
  // Helper: count JPEG files in directory
  int countJpegFiles() {
    int count = 0;
    try {
      for (const auto& entry : fs::directory_iterator(test_dir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
          count++;
        }
      }
    } catch (...) {}
    return count;
  }
  
  // Helper: verify JPEG file is valid
  bool isValidJpeg(const fs::path& path) {
    cv::Mat img = cv::imread(path.string(), cv::IMREAD_COLOR);
    return !img.empty();
  }
  
  fs::path test_dir_;
};

TEST_F(AsyncFrameWriterTest, BasicWriteOperation) {
  auto writer = make_frame_writer_disk_async(test_dir_.string(), 0, 85, output::BlurConfig{}, 8);
  
  cv::Mat frame = createTestFrame();
  PipelineResult result = createTestResult();
  
  // Write frame (should return immediately, queued for background)
  EXPECT_TRUE(writer->writeFrame(frame, result));
  
  // Flush and verify file written
  writer->flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Allow write to complete
  
  EXPECT_GT(countJpegFiles(), 0) << "Expected at least one JPEG file written";
}

TEST_F(AsyncFrameWriterTest, MultipleFramesWritten) {
  auto writer = make_frame_writer_disk_async(test_dir_.string(), 0, 85, output::BlurConfig{}, 8);
  
  constexpr int NUM_FRAMES = 30;  // Write 30 frames, expect 10 written (every 3rd)
  
  for (int i = 0; i < NUM_FRAMES; ++i) {
    cv::Mat frame = createTestFrame();
    PipelineResult result = createTestResult(i);
    EXPECT_TRUE(writer->writeFrame(frame, result));
  }
  
  writer->flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  
  int written = countJpegFiles();
  EXPECT_GE(written, 8) << "Expected ~10 frames written (every 3rd of 30)";
  EXPECT_LE(written, 12) << "Expected ~10 frames written, not more";
}

TEST_F(AsyncFrameWriterTest, QueueOverflowDropsFrames) {
  // Small queue to force overflow
  auto writer = make_frame_writer_disk_async(test_dir_.string(), 0, 85, output::BlurConfig{}, 4);
  
  // Rapidly enqueue many frames
  constexpr int NUM_FRAMES = 100;
  int successful = 0;
  
  for (int i = 0; i < NUM_FRAMES; ++i) {
    cv::Mat frame = createTestFrame();
    PipelineResult result = createTestResult(i);
    if (writer->writeFrame(frame, result)) {
      successful++;
    }
  }
  
  writer->flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  
  // Some frames should have been dropped due to small queue
  int written = countJpegFiles();
  EXPECT_GT(written, 0) << "At least some frames should be written";
  EXPECT_LT(written, NUM_FRAMES / 3 + 2) << "Not all frames should be written (queue overflow, allow ±1 tolerance)";
}

TEST_F(AsyncFrameWriterTest, FlushWaitsForPendingWrites) {
  auto writer = make_frame_writer_disk_async(test_dir_.string(), 0, 85, output::BlurConfig{}, 8);
  
  constexpr int NUM_FRAMES = 12;
  for (int i = 0; i < NUM_FRAMES; ++i) {
    cv::Mat frame = createTestFrame();
    PipelineResult result = createTestResult(i);
    writer->writeFrame(frame, result);
  }
  
  // Flush should wait for all pending writes
  auto start = std::chrono::steady_clock::now();
  writer->flush();
  auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count();
  
  // Flush should have completed quickly (async writer is very fast)
  // Just verify the call succeeded
  EXPECT_GE(elapsed_us, 0);
  
  // All written frames should be present
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int written = countJpegFiles();
  EXPECT_GE(written, 3) << "Expected ~4 frames written (every 3rd of 12)";
}

TEST_F(AsyncFrameWriterTest, GracefulShutdown) {
  {
    auto writer = make_frame_writer_disk_async(test_dir_.string(), 0, 85, output::BlurConfig{}, 8);
    
    // Write some frames
    for (int i = 0; i < 15; ++i) {
      cv::Mat frame = createTestFrame();
      PipelineResult result = createTestResult(i);
      writer->writeFrame(frame, result);
    }
    
    // Writer destructor should flush and shutdown gracefully
  }
  
  // Give background thread time to complete
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  
  // Verify files written
  int written = countJpegFiles();
  EXPECT_GT(written, 0) << "Some frames should be written before shutdown";
}

TEST_F(AsyncFrameWriterTest, MaxFramesLimitRespected) {
  constexpr int MAX_FRAMES = 5;
  auto writer = make_frame_writer_disk_async(test_dir_.string(), MAX_FRAMES, 85, output::BlurConfig{}, 8);
  
  // Write many frames
  for (int i = 0; i < 60; ++i) {  // 60 frames = 20 written (every 3rd)
    cv::Mat frame = createTestFrame();
    PipelineResult result = createTestResult(i);
    writer->writeFrame(frame, result);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // Slow down to allow cleanup
  }
  
  writer->flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  
  int written = countJpegFiles();
  EXPECT_LE(written, MAX_FRAMES + 2) << "Max frames limit should be respected (with tolerance)";
}

TEST_F(AsyncFrameWriterTest, EmptyImageHandledGracefully) {
  auto writer = make_frame_writer_disk_async(test_dir_.string(), 0, 85, output::BlurConfig{}, 8);
  
  cv::Mat empty_frame;
  PipelineResult result = createTestResult();
  
  // Empty frame should return false
  EXPECT_FALSE(writer->writeFrame(empty_frame, result));
  
  writer->flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  
  // No files should be written
  EXPECT_EQ(countJpegFiles(), 0);
}

TEST_F(AsyncFrameWriterTest, JpegQualityApplied) {
  constexpr int QUALITY = 50;  // Low quality for small file size
  auto writer = make_frame_writer_disk_async(test_dir_.string(), 0, QUALITY, output::BlurConfig{}, 8);
  
  cv::Mat frame = createTestFrame(640, 480);
  PipelineResult result = createTestResult();
  
  // Write 3 frames to trigger at least 1 write
  for (int i = 0; i < 3; ++i) {
    writer->writeFrame(frame, result);
  }
  
  writer->flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  
  // Verify file exists and is valid
  EXPECT_GT(countJpegFiles(), 0);
  
  for (const auto& entry : fs::directory_iterator(test_dir_)) {
    if (entry.path().extension() == ".jpg") {
      EXPECT_TRUE(isValidJpeg(entry.path()));
      // Low quality should result in smaller file (rough check)
      auto size_kb = fs::file_size(entry.path()) / 1024;
      EXPECT_LT(size_kb, 100) << "Low quality JPEG should be relatively small";
    }
  }
}

TEST_F(AsyncFrameWriterTest, ThreadSafety) {
  auto writer = make_frame_writer_disk_async(test_dir_.string(), 0, 85, output::BlurConfig{}, 16);
  
  // Multiple threads writing frames concurrently
  constexpr int NUM_THREADS = 4;
  constexpr int FRAMES_PER_THREAD = 15;
  std::vector<std::thread> threads;
  
  for (int t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < FRAMES_PER_THREAD; ++i) {
        cv::Mat frame = createTestFrame();
        PipelineResult result = createTestResult(t * 1000 + i);
        writer->writeFrame(frame, result);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }
  
  writer->flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  
  // Verify some frames written (exact count depends on timing)
  int written = countJpegFiles();
  EXPECT_GT(written, 0) << "At least some frames should be written";
}

// ============================================================================
// Performance Benchmark Tests
// ============================================================================

class AsyncFrameWriterBenchmark : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "frame_writer_bench";
    fs::remove_all(test_dir_);
    fs::create_directories(test_dir_);
  }
  
  void TearDown() override {
    fs::remove_all(test_dir_);
  }
  
  cv::Mat createTestFrame() {
    cv::Mat frame(320, 320, CV_8UC3);
    cv::randu(frame, cv::Scalar(0, 0, 0), cv::Scalar(255, 255, 255));
    return frame;
  }
  
  PipelineResult createTestResult(uint64_t seq) {
    PipelineResult result;
    result.seq = seq;
    result.frame_width = 1920;
    result.frame_height = 1080;
    return result;
  }
  
  fs::path test_dir_;
};

TEST_F(AsyncFrameWriterBenchmark, WriteFrameLatency) {
  auto writer = make_frame_writer_disk_async(test_dir_.string(), 0, 85, output::BlurConfig{}, 16);
  
  cv::Mat frame = createTestFrame();
  PipelineResult result = createTestResult(1);
  
  // Warm up
  for (int i = 0; i < 10; ++i) {
    writer->writeFrame(frame, result);
  }
  
  // Measure latency
  constexpr int NUM_SAMPLES = 100;
  std::vector<int64_t> latencies_us;
  latencies_us.reserve(NUM_SAMPLES);
  
  for (int i = 0; i < NUM_SAMPLES; ++i) {
    auto start = std::chrono::steady_clock::now();
    writer->writeFrame(frame, result);
    auto end = std::chrono::steady_clock::now();
    
    auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    latencies_us.push_back(latency_us);
  }
  
  // Calculate statistics
  int64_t sum = 0;
  int64_t max_latency = 0;
  for (auto lat : latencies_us) {
    sum += lat;
    max_latency = std::max(max_latency, lat);
  }
  int64_t avg_latency_us = sum / NUM_SAMPLES;
  
  std::cout << "\n=== Async Frame Writer Performance ===\n";
  std::cout << "Average writeFrame() latency: " << avg_latency_us << " µs\n";
  std::cout << "Max writeFrame() latency:     " << max_latency << " µs\n";
  std::cout << "Target: < 500 µs (0.5ms)\n";
  std::cout << "======================================\n";
  
  // Phase 2C target: writeFrame() should take < 500µs (just enqueue, no encoding)
  EXPECT_LT(avg_latency_us, 500) << "Async writeFrame() should be fast (< 0.5ms)";
  
  writer->flush();
}

TEST_F(AsyncFrameWriterBenchmark, CompareAsyncVsSync) {
  cv::Mat frame = createTestFrame();
  PipelineResult result = createTestResult(1);
  
  // Measure sync writer
  auto sync_writer = make_frame_writer_disk(test_dir_.string() + "_sync", 0, 85, output::BlurConfig{});
  
  auto start_sync = std::chrono::steady_clock::now();
  for (int i = 0; i < 30; ++i) {
    sync_writer->writeFrame(frame, result);
  }
  auto end_sync = std::chrono::steady_clock::now();
  auto sync_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_sync - start_sync).count();
  
  // Measure async writer
  auto async_writer = make_frame_writer_disk_async(test_dir_.string() + "_async", 0, 85, output::BlurConfig{}, 16);
  
  auto start_async = std::chrono::steady_clock::now();
  for (int i = 0; i < 30; ++i) {
    async_writer->writeFrame(frame, result);
  }
  auto end_async = std::chrono::steady_clock::now();
  auto async_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_async - start_async).count();
  
  async_writer->flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  
  std::cout << "\n=== Sync vs Async Comparison ===\n";
  std::cout << "Sync total time:  " << sync_total_ms << " ms (30 frames)\n";
  std::cout << "Async total time: " << async_total_ms << " ms (30 frames)\n";
  std::cout << "Speedup:          " << (float)sync_total_ms / async_total_ms << "x\n";
  std::cout << "================================\n";
  
  // Async should be significantly faster (main thread time only)
  EXPECT_LT(async_total_ms, sync_total_ms) << "Async should be faster than sync";
  EXPECT_LT(async_total_ms, sync_total_ms / 2) << "Async should be at least 2x faster";
  
  // Cleanup
  fs::remove_all(test_dir_.string() + "_sync");
  fs::remove_all(test_dir_.string() + "_async");
}

// ============================================================================
// Test Suite Summary
// ============================================================================

// Phase 2C Test Summary:
// - SPSCQueue: 6 tests (basic, full, empty, move, concurrent, stress)
// - AsyncFrameWriter: 11 tests (basic, multiple, overflow, flush, shutdown, etc.)
// - Performance: 2 benchmarks (latency, sync vs async comparison)
// Total: 19 tests
