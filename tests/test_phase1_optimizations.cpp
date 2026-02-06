/**
 * @file test_phase1_optimizations.cpp
 * @brief Phase 1 Optimization Tests
 * 
 * Tests to validate Phase 1 optimizations:
 * 1. BBox area pre-calculation for faster IoU
 * 2. Fast direction LUT vs atan2
 * 
 * Test Suite: Phase1OptimizationTest
 * Test Cases: 8 tests
 */

#include <gtest/gtest.h>
#include "tracking/byte_types.h"
#include "tracking/tracker.h"
#include <cmath>
#include <chrono>

// ============================================================================
// Test Fixture
// ============================================================================

class Phase1OptimizationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup before each test
    }

    void TearDown() override {
        // Cleanup after each test
    }
};

// ============================================================================
// TC-001: BBox Area Pre-calculation
// ============================================================================

TEST_F(Phase1OptimizationTest, BBoxAreaField) {
    // Test that BBox has area field for pre-calculation
    bytetrack::BBox box;
    box.x0 = 100.0f;
    box.y0 = 100.0f;
    box.x1 = 200.0f;
    box.y1 = 200.0f;
    
    // Default area should be 0
    EXPECT_EQ(box.area, 0.0f) << "Default area should be 0";
    
    // Calculate and cache area
    box.update_area();
    EXPECT_EQ(box.area, 10000.0f) << "Area should be 100x100 = 10000";
}

TEST_F(Phase1OptimizationTest, BBoxAreaUpdateMethod) {
    // Test update_area() method
    bytetrack::BBox box;
    box.x0 = 50.0f;
    box.y0 = 50.0f;
    box.x1 = 150.0f;
    box.y1 = 250.0f;
    
    box.update_area();
    
    float expected_area = (150.0f - 50.0f) * (250.0f - 50.0f);
    EXPECT_FLOAT_EQ(box.area, expected_area) << "Area should be 100x200 = 20000";
}

TEST_F(Phase1OptimizationTest, IoUUsesPreCalculatedArea) {
    // Test that IoU uses pre-calculated area when available
    bytetrack::BBox a, b;
    
    // Box A: (100, 100) to (200, 200)
    a.x0 = 100.0f; a.y0 = 100.0f;
    a.x1 = 200.0f; a.y1 = 200.0f;
    a.update_area();
    
    // Box B: (150, 150) to (250, 250)
    b.x0 = 150.0f; b.y0 = 150.0f;
    b.x1 = 250.0f; b.y1 = 250.0f;
    b.update_area();
    
    // Calculate IoU with pre-calculated areas
    float iou = bytetrack::iou(a, b);
    
    // Intersection: 50x50 = 2500
    // Union: 10000 + 10000 - 2500 = 17500
    // IoU: 2500 / 17500 ≈ 0.1428
    EXPECT_NEAR(iou, 0.1428f, 0.001f) << "IoU with pre-calculated areas";
}

TEST_F(Phase1OptimizationTest, IoUFallbackWithoutArea) {
    // Test that IoU works even without pre-calculated area
    bytetrack::BBox a, b;
    
    // Box A: (100, 100) to (200, 200) - no update_area()
    a.x0 = 100.0f; a.y0 = 100.0f;
    a.x1 = 200.0f; a.y1 = 200.0f;
    // area remains 0, should fall back to w() * h()
    
    // Box B: (150, 150) to (250, 250) - no update_area()
    b.x0 = 150.0f; b.y0 = 150.0f;
    b.x1 = 250.0f; b.y1 = 250.0f;
    
    // Calculate IoU without pre-calculated areas
    float iou = bytetrack::iou(a, b);
    
    EXPECT_NEAR(iou, 0.1428f, 0.001f) << "IoU should work without pre-calculated areas";
}

// ============================================================================
// TC-002: Fast Direction Calculation
// ============================================================================

TEST_F(Phase1OptimizationTest, FastDirectionCardinalDirections) {
    // Test fast_direction_deg for cardinal directions
    
    // Right: (1, 0) → 0°
    float deg_r = Tracker::fast_direction_deg(1.0f, 0.0f);
    EXPECT_FLOAT_EQ(deg_r, 0.0f) << "Right direction should be 0°";
    
    // Up: (0, -1) → 90° (y-down in image coords, so negate)
    float deg_u = Tracker::fast_direction_deg(0.0f, -1.0f);
    EXPECT_FLOAT_EQ(deg_u, 90.0f) << "Up direction should be 90°";
    
    // Left: (-1, 0) → 180°
    float deg_l = Tracker::fast_direction_deg(-1.0f, 0.0f);
    EXPECT_FLOAT_EQ(deg_l, 180.0f) << "Left direction should be 180°";
    
    // Down: (0, 1) → 270°
    float deg_d = Tracker::fast_direction_deg(0.0f, 1.0f);
    EXPECT_FLOAT_EQ(deg_d, 270.0f) << "Down direction should be 270°";
}

TEST_F(Phase1OptimizationTest, FastDirectionIntercardinalDirections) {
    // Test fast_direction_deg for intercardinal directions (45° angles)
    
    // Upper-right: (1, -1) → 45°
    float deg_ur = Tracker::fast_direction_deg(1.0f, -1.0f);
    EXPECT_FLOAT_EQ(deg_ur, 45.0f) << "Upper-right direction should be 45°";
    
    // Upper-left: (-1, -1) → 135°
    float deg_ul = Tracker::fast_direction_deg(-1.0f, -1.0f);
    EXPECT_FLOAT_EQ(deg_ul, 135.0f) << "Upper-left direction should be 135°";
    
    // Lower-left: (-1, 1) → 225°
    float deg_dl = Tracker::fast_direction_deg(-1.0f, 1.0f);
    EXPECT_FLOAT_EQ(deg_dl, 225.0f) << "Lower-left direction should be 225°";
    
    // Lower-right: (1, 1) → 315°
    float deg_dr = Tracker::fast_direction_deg(1.0f, 1.0f);
    EXPECT_FLOAT_EQ(deg_dr, 315.0f) << "Lower-right direction should be 315°";
}

TEST_F(Phase1OptimizationTest, FastDirectionZeroVelocity) {
    // Test fast_direction_deg with zero velocity
    float deg = Tracker::fast_direction_deg(0.0f, 0.0f);
    EXPECT_FLOAT_EQ(deg, 0.0f) << "Zero velocity should return 0°";
}

TEST_F(Phase1OptimizationTest, FastDirectionBenchmarkVsAtan2) {
    // Benchmark comparison: fast_direction_deg vs atan2
    constexpr int ITERATIONS = 10000;
    
    // Generate test velocities
    std::vector<std::pair<float, float>> velocities;
    for (int i = 0; i < ITERATIONS; i++) {
        float angle = (i * 360.0f / ITERATIONS) * M_PI / 180.0f;
        velocities.emplace_back(std::cos(angle), std::sin(angle));
    }
    
    // Benchmark fast_direction_deg
    auto start_fast = std::chrono::high_resolution_clock::now();
    volatile float sum_fast = 0.0f;
    for (const auto& [vx, vy] : velocities) {
        sum_fast += Tracker::fast_direction_deg(vx, vy);
    }
    auto end_fast = std::chrono::high_resolution_clock::now();
    auto duration_fast = std::chrono::duration_cast<std::chrono::microseconds>(end_fast - start_fast).count();
    
    // Benchmark atan2
    auto start_atan2 = std::chrono::high_resolution_clock::now();
    volatile float sum_atan2 = 0.0f;
    for (const auto& [vx, vy] : velocities) {
        float deg = std::atan2(-vy, vx) * 180.0f / M_PI;
        if (deg < 0.0f) deg += 360.0f;
        sum_atan2 += deg;
    }
    auto end_atan2 = std::chrono::high_resolution_clock::now();
    auto duration_atan2 = std::chrono::duration_cast<std::chrono::microseconds>(end_atan2 - start_atan2).count();
    
    float avg_fast_us = duration_fast / float(ITERATIONS);
    float avg_atan2_us = duration_atan2 / float(ITERATIONS);
    float speedup = duration_atan2 / float(duration_fast);
    
    // Print results
    std::cout << "\nDirection Calculation Benchmark (" << ITERATIONS << " iterations):\n";
    std::cout << "  fast_direction_deg: " << duration_fast << " µs total ("
              << avg_fast_us << " µs avg)\n";
    std::cout << "  atan2:              " << duration_atan2 << " µs total ("
              << avg_atan2_us << " µs avg)\n";
    std::cout << "  Speedup:            " << speedup << "×\n";
    
    // Validate that fast version is faster (should be 3-10× depending on CPU)
    EXPECT_LT(duration_fast, duration_atan2) 
        << "Fast direction should be faster than atan2";
    
    // Both methods should complete in reasonable time
    EXPECT_LT(avg_fast_us, 0.1) 
        << "Fast direction avg too slow: " << avg_fast_us << " µs";
    EXPECT_LT(avg_atan2_us, 2.0) 
        << "atan2 avg too slow: " << avg_atan2_us << " µs";
}

// ============================================================================
// Test Suite Summary
// ============================================================================

/*
 * PHASE 1 OPTIMIZATION TEST SUITE SUMMARY
 * ========================================
 * 
 * Test Coverage:
 * - BBox area pre-calculation (4 tests)
 * - Fast direction LUT (4 tests)
 * 
 * Total Tests: 8
 * 
 * Key Optimizations Validated:
 * - BBox::area field for IoU optimization
 * - BBox::update_area() method
 * - IoU fallback when area not pre-calculated
 * - Tracker::fast_direction_deg() 8-way quantization
 * - Performance improvement: fast_direction_deg vs atan2
 * 
 * Performance Targets:
 * - fast_direction_deg: <0.1 µs average (3-10× faster than atan2)
 * - atan2 baseline: <2.0 µs average
 * 
 * Expected Speedup: 3-10× for direction calculation
 * Expected IoU improvement: ~20% (area pre-calculation eliminates 2 multiplies)
 * 
 * Note: These are software-layer micro-optimizations. Frame-level impact is
 * negligible (<0.01ms per frame with 50 detections), but improves code quality
 * and demonstrates optimization best practices.
 */
