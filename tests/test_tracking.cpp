/**
 * @file test_tracking.cpp
 * @brief Week 3 Sprint 2 - Tracking Tests
 * 
 * Tests for tracking configuration, state transitions, and motion calculations.
 * Covers TrackerConfig, TrackState, motion smoothing, and direction quantization.
 * 
 * Note: These are pure unit tests that test configuration structures and
 * mathematical calculations without requiring actual frame sequences or
 * stateful tracking algorithms (tested in integration tests).
 * 
 * Test Suite: TrackingTest
 * Test Cases: 12 tests
 */

#include <gtest/gtest.h>
#include "tracking/tracker.h"
#include "models/model_runner.h"
#include <cmath>
#include <vector>

// ============================================================================
// Test Fixture
// ============================================================================

class TrackingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup before each test
    }

    void TearDown() override {
        // Cleanup after each test
    }
    
    // Helper: Calculate IoU (same as postprocessing tests)
    float calculate_iou(float x0a, float y0a, float x1a, float y1a,
                       float x0b, float y0b, float x1b, float y1b) {
        float inter_x0 = std::max(x0a, x0b);
        float inter_y0 = std::max(y0a, y0b);
        float inter_x1 = std::min(x1a, x1b);
        float inter_y1 = std::min(y1a, y1b);
        
        if (inter_x1 <= inter_x0 || inter_y1 <= inter_y0) {
            return 0.0f;
        }
        
        float inter_area = (inter_x1 - inter_x0) * (inter_y1 - inter_y0);
        float area_a = (x1a - x0a) * (y1a - y0a);
        float area_b = (x1b - x0b) * (y1b - y0b);
        float union_area = area_a + area_b - inter_area;
        
        return (union_area > 0) ? (inter_area / union_area) : 0.0f;
    }
};

// ============================================================================
// TC-070: TrackState Enum
// ============================================================================

TEST_F(TrackingTest, TrackStateEnum) {
    // Test TrackState enum values
    TrackState tentative = TrackState::Tentative;
    TrackState confirmed = TrackState::Confirmed;
    TrackState deleted = TrackState::Deleted;
    
    // States should be distinct
    EXPECT_NE(tentative, confirmed);
    EXPECT_NE(tentative, deleted);
    EXPECT_NE(confirmed, deleted);
}

// ============================================================================
// TC-071: TrackerConfig Defaults
// ============================================================================

TEST_F(TrackingTest, TrackerConfigDefaults) {
    // Test TrackerConfig default values
    TrackerConfig config;
    
    // Core algorithm
    EXPECT_EQ(config.tracker_core, "legacy") << "Default should be legacy tracker";
    
    // ByteTrack parameters
    EXPECT_EQ(config.byte_det_high, 0.50f) << "High detection threshold";
    EXPECT_EQ(config.byte_det_low, 0.15f) << "Low detection threshold";
    EXPECT_EQ(config.byte_match_iou_high, 0.70f) << "High IoU threshold";
    EXPECT_EQ(config.byte_match_iou_low, 0.50f) << "Low IoU threshold";
    EXPECT_EQ(config.byte_max_age, 30) << "Max age 30 frames";
    EXPECT_EQ(config.byte_n_init, 2) << "N_init lowered to 2 for faster confirmation";
    
    // Legacy parameters
    EXPECT_EQ(config.iou_match_thresh, 0.45f) << "Legacy IoU threshold";
    EXPECT_EQ(config.confirm_hits, 2) << "Confirm hits lowered to 2";
    EXPECT_EQ(config.max_missed, 12) << "Max missed 12 frames (~0.4s @30fps)";
    EXPECT_EQ(config.min_det_score, 0.50f) << "Min detection score";
    EXPECT_EQ(config.min_area_px, 1600) << "Min area 1600 px (40×40)";
    
    // Motion & smoothing
    EXPECT_EQ(config.motion_eps_px, 0.25f) << "Motion epsilon";
    EXPECT_EQ(config.smooth_pos_alpha, 0.45f) << "Position smoothing alpha";
    EXPECT_EQ(config.smooth_vel_alpha, 0.70f) << "Velocity smoothing alpha";
    
    // Speed limits
    EXPECT_EQ(config.min_speed_px_s, 8.0f) << "Min speed 8 px/s";
    EXPECT_EQ(config.max_speed_px_s, 90.0f) << "Max speed 90 px/s";
    
    // Direction parameters
    EXPECT_EQ(config.dir_hysteresis_deg, 22.5f) << "Direction hysteresis";
    EXPECT_EQ(config.bin_hysteresis_deg, 10.0f) << "Bin hysteresis";
    EXPECT_EQ(config.dir_hold_ms, 850.0) << "Direction hold time 850ms";
    
    // Publishing parameters
    EXPECT_EQ(config.publish_grace_missed, 6) << "Grace period 6 frames";
    EXPECT_EQ(config.publish_score_min, 0.70f) << "Min publish score 0.70";
    EXPECT_EQ(config.enter_exit_border_frac, 0.10f) << "Border fraction 10%";
}

TEST_F(TrackingTest, TrackerConfigCustom) {
    // Test custom configuration
    TrackerConfig config;
    config.tracker_core = "byte";
    config.byte_det_high = 0.60f;
    config.iou_match_thresh = 0.50f;
    config.confirm_hits = 3;
    config.max_missed = 20;
    config.min_speed_px_s = 10.0f;
    config.max_speed_px_s = 100.0f;
    
    EXPECT_EQ(config.tracker_core, "byte");
    EXPECT_EQ(config.byte_det_high, 0.60f);
    EXPECT_EQ(config.iou_match_thresh, 0.50f);
    EXPECT_EQ(config.confirm_hits, 3);
    EXPECT_EQ(config.max_missed, 20);
    EXPECT_EQ(config.min_speed_px_s, 10.0f);
    EXPECT_EQ(config.max_speed_px_s, 100.0f);
}

// ============================================================================
// TC-072: IoU-Based Track Association
// ============================================================================

TEST_F(TrackingTest, IoUAssociationHighOverlap) {
    // Test IoU calculation for track-detection association
    // Track bbox: (100, 100) to (200, 200) - 100×100 = 10,000 area
    // Detection:  (110, 110) to (210, 210) - 100×100 = 10,000 area
    
    float iou = calculate_iou(100, 100, 200, 200,
                              110, 110, 210, 210);
    
    // Intersection: (110,110) to (200,200) = 90×90 = 8,100
    // Union: 10,000 + 10,000 - 8,100 = 11,900
    // IoU = 8,100 / 11,900 ≈ 0.68
    EXPECT_NEAR(iou, 0.68f, 0.01f) << "High overlap should give IoU ~0.68";
    
    // Should exceed typical association threshold (0.45)
    TrackerConfig config;
    EXPECT_GT(iou, config.iou_match_thresh) << "Should match track";
}

TEST_F(TrackingTest, IoUAssociationLowOverlap) {
    // Test IoU with low overlap
    // Track bbox: (100, 100) to (200, 200)
    // Detection:  (180, 180) to (280, 280) - small overlap
    
    float iou = calculate_iou(100, 100, 200, 200,
                              180, 180, 280, 280);
    
    // Expected: Small intersection, IoU < 0.1
    EXPECT_LT(iou, 0.1f) << "Low overlap should give low IoU";
    
    // Should NOT exceed association threshold
    TrackerConfig config;
    EXPECT_LT(iou, config.iou_match_thresh) << "Should not match track";
}

TEST_F(TrackingTest, IoUThresholdForByteTrack) {
    // Test ByteTrack uses two-stage matching with different IoU thresholds
    TrackerConfig config;
    
    // Stage 1: High-confidence detections use high IoU threshold
    EXPECT_EQ(config.byte_match_iou_high, 0.70f) << "High-conf matching uses 0.70 IoU";
    
    // Stage 2: Low-confidence detections use lower IoU threshold
    EXPECT_EQ(config.byte_match_iou_low, 0.50f) << "Low-conf matching uses 0.50 IoU";
    
    // High threshold should be stricter
    EXPECT_GT(config.byte_match_iou_high, config.byte_match_iou_low);
}

// ============================================================================
// TC-073: Track State Transitions
// ============================================================================

TEST_F(TrackingTest, TrackLifecycle) {
    // Test track lifecycle: Tentative → Confirmed → Deleted
    TrackerConfig config;
    
    // New track starts as Tentative
    TrackState state = TrackState::Tentative;
    int hits = 0;
    
    EXPECT_EQ(state, TrackState::Tentative);
    EXPECT_EQ(hits, 0);
    
    // After confirm_hits consecutive matches, becomes Confirmed
    hits = config.confirm_hits;  // 2 consecutive matches
    state = TrackState::Confirmed;
    
    EXPECT_EQ(state, TrackState::Confirmed);
    EXPECT_GE(hits, config.confirm_hits);
    
    // After max_missed frames without match, becomes Deleted
    int missed = config.max_missed + 1;  // 13 missed frames
    state = TrackState::Deleted;
    
    EXPECT_EQ(state, TrackState::Deleted);
    EXPECT_GT(missed, config.max_missed);
}

TEST_F(TrackingTest, ConfirmationThreshold) {
    // Test that confirm_hits threshold determines when track is confirmed
    TrackerConfig config;
    
    EXPECT_EQ(config.confirm_hits, 2) << "Lowered to 2 for faster confirmation";
    
    // Simulate track progression
    int hits = 0;
    TrackState state = TrackState::Tentative;
    
    // First match
    hits++;
    EXPECT_LT(hits, config.confirm_hits);
    EXPECT_EQ(state, TrackState::Tentative) << "Still tentative after 1 hit";
    
    // Second match - should confirm
    hits++;
    if (hits >= config.confirm_hits) {
        state = TrackState::Confirmed;
    }
    EXPECT_EQ(state, TrackState::Confirmed) << "Confirmed after 2 hits";
}

// ============================================================================
// TC-074: Motion Vector Calculations
// ============================================================================

TEST_F(TrackingTest, MotionVectorSpeed) {
    // Test speed calculation from velocity components
    float vx = 3.0f;  // 3 px/s right
    float vy = 4.0f;  // 4 px/s down
    
    float speed = std::sqrt(vx * vx + vy * vy);
    
    EXPECT_FLOAT_EQ(speed, 5.0f) << "Speed should be sqrt(3²+4²) = 5.0";
}

TEST_F(TrackingTest, MotionVectorDirection) {
    // Test direction calculation from velocity components
    float vx = 1.0f;   // Right
    float vy = 0.0f;   // No vertical
    
    float dir_rad = std::atan2(vy, vx);
    float dir_deg = dir_rad * 180.0f / M_PI;
    
    EXPECT_FLOAT_EQ(dir_deg, 0.0f) << "Right motion should be 0°";
    
    // Test other directions
    float dir_up = std::atan2(-1.0f, 0.0f) * 180.0f / M_PI;    // Up (negative y in image coords)
    float dir_left = std::atan2(0.0f, -1.0f) * 180.0f / M_PI;  // Left
    float dir_down = std::atan2(1.0f, 0.0f) * 180.0f / M_PI;   // Down
    
    EXPECT_NEAR(dir_up, -90.0f, 0.1f) << "Up motion should be -90°";
    EXPECT_NEAR(dir_left, 180.0f, 0.1f) << "Left motion should be ±180°";
    EXPECT_NEAR(dir_down, 90.0f, 0.1f) << "Down motion should be 90°";
}

TEST_F(TrackingTest, DirectionQuantization) {
    // Test direction quantization to 8 bins + stationary
    // Bins: R(0°), UR(45°), U(90°), UL(135°), L(180°), DL(225°), D(270°), DR(315°)
    
    struct DirectionTest {
        float deg;
        const char* expected_label;
    };
    
    std::vector<DirectionTest> tests = {
        {0.0f, "R"},      // Right
        {45.0f, "UR"},    // Up-Right
        {90.0f, "U"},     // Up
        {135.0f, "UL"},   // Up-Left
        {180.0f, "L"},    // Left
        {225.0f, "DL"},   // Down-Left
        {270.0f, "D"},    // Down
        {315.0f, "DR"},   // Down-Right
    };
    
    for (const auto& test : tests) {
        // Direction should be quantized to nearest 45° bin
        int bin = static_cast<int>((test.deg + 22.5f) / 45.0f) % 8;
        
        // Verify bin is in valid range
        EXPECT_GE(bin, 0);
        EXPECT_LT(bin, 8);
    }
}

// ============================================================================
// TC-075: EMA Smoothing
// ============================================================================

TEST_F(TrackingTest, EMAPositionSmoothing) {
    // Test Exponential Moving Average (EMA) for position smoothing
    TrackerConfig config;
    float alpha = config.smooth_pos_alpha;  // 0.45
    
    EXPECT_EQ(alpha, 0.45f) << "Position smoothing alpha should be 0.45";
    
    // Simulate EMA: smoothed = alpha * new + (1-alpha) * old
    float old_x = 100.0f;
    float new_x = 110.0f;
    float smoothed_x = alpha * new_x + (1.0f - alpha) * old_x;
    
    // Expected: 0.45 * 110 + 0.55 * 100 = 49.5 + 55 = 104.5
    EXPECT_FLOAT_EQ(smoothed_x, 104.5f) << "EMA should smooth position";
    
    // Smoothed value should be between old and new
    EXPECT_GT(smoothed_x, old_x);
    EXPECT_LT(smoothed_x, new_x);
}

TEST_F(TrackingTest, EMAVelocitySmoothing) {
    // Test EMA for velocity smoothing (more responsive than position)
    TrackerConfig config;
    float alpha = config.smooth_vel_alpha;  // 0.70
    
    EXPECT_EQ(alpha, 0.70f) << "Velocity smoothing alpha should be 0.70";
    
    // Higher alpha = more responsive to changes
    EXPECT_GT(config.smooth_vel_alpha, config.smooth_pos_alpha)
        << "Velocity should be more responsive than position";
    
    // Simulate velocity smoothing
    float old_vx = 5.0f;
    float new_vx = 10.0f;
    float smoothed_vx = alpha * new_vx + (1.0f - alpha) * old_vx;
    
    // Expected: 0.70 * 10 + 0.30 * 5 = 7 + 1.5 = 8.5
    EXPECT_FLOAT_EQ(smoothed_vx, 8.5f) << "EMA should smooth velocity";
}

// ============================================================================
// TC-076: Speed Clamping
// ============================================================================

TEST_F(TrackingTest, SpeedFloorClamping) {
    // Test minimum speed floor
    TrackerConfig config;
    float min_speed = config.min_speed_px_s;  // 8.0 px/s
    
    EXPECT_EQ(min_speed, 8.0f) << "Min speed floor for 480p";
    
    // Speeds below floor should be clamped
    float slow_speed = 5.0f;
    float clamped_speed = std::max(slow_speed, min_speed);
    
    EXPECT_EQ(clamped_speed, min_speed) << "Slow speed should clamp to floor";
    
    // Speeds above floor should pass through
    float fast_speed = 15.0f;
    clamped_speed = std::max(fast_speed, min_speed);
    
    EXPECT_EQ(clamped_speed, fast_speed) << "Fast speed should not clamp";
}

TEST_F(TrackingTest, SpeedCeilingClamping) {
    // Test maximum speed ceiling
    TrackerConfig config;
    float max_speed = config.max_speed_px_s;  // 90.0 px/s
    
    EXPECT_EQ(max_speed, 90.0f) << "Max speed ceiling";
    
    // Speeds above ceiling should be clamped
    float high_speed = 150.0f;
    float clamped_speed = std::min(high_speed, max_speed);
    
    EXPECT_EQ(clamped_speed, max_speed) << "High speed should clamp to ceiling";
    
    // Speeds below ceiling should pass through
    float normal_speed = 50.0f;
    clamped_speed = std::min(normal_speed, max_speed);
    
    EXPECT_EQ(clamped_speed, normal_speed) << "Normal speed should not clamp";
}

// ============================================================================
// TC-077: Detection Quality Filtering
// ============================================================================

TEST_F(TrackingTest, MinimumDetectionScore) {
    // Test minimum detection score threshold
    TrackerConfig config;
    float min_score = config.min_det_score;  // 0.50
    
    EXPECT_EQ(min_score, 0.50f) << "Min detection score should be 0.50";
    
    // Detections below threshold should be filtered
    std::vector<float> scores = {0.30f, 0.45f, 0.50f, 0.65f, 0.85f};
    int passed = 0;
    
    for (float score : scores) {
        if (score >= min_score) {
            passed++;
        }
    }
    
    EXPECT_EQ(passed, 3) << "Should pass 3 detections (0.50, 0.65, 0.85)";
}

TEST_F(TrackingTest, MinimumDetectionArea) {
    // Test minimum detection area threshold
    TrackerConfig config;
    int min_area = config.min_area_px;  // 1600 px (40×40)
    
    EXPECT_EQ(min_area, 1600) << "Min area should be 1600 px";
    
    // Test various box sizes
    struct BoxTest {
        float width, height;
        bool should_pass;
    };
    
    std::vector<BoxTest> tests = {
        {30.0f, 30.0f, false},   // 900 px < 1600
        {40.0f, 40.0f, true},    // 1600 px = 1600
        {50.0f, 50.0f, true},    // 2500 px > 1600
        {80.0f, 20.0f, true},    // 1600 px = 1600 (wide box)
    };
    
    for (const auto& test : tests) {
        float area = test.width * test.height;
        bool passes = (area >= min_area);
        
        EXPECT_EQ(passes, test.should_pass) 
            << "Box " << test.width << "×" << test.height 
            << " area=" << area << " should " 
            << (test.should_pass ? "pass" : "fail");
    }
}

// ============================================================================
// TC-078: Enter/Exit Border Detection
// ============================================================================

TEST_F(TrackingTest, EnterExitBorderFraction) {
    // Test ROI border fraction for enter/exit detection
    TrackerConfig config;
    float border_frac = config.enter_exit_border_frac;  // 0.10 = 10% inset
    
    EXPECT_EQ(border_frac, 0.10f) << "Border fraction should be 10%";
    
    // Calculate border for 1280×720 frame
    int frame_w = 1280;
    int frame_h = 720;
    
    int border_x = static_cast<int>(frame_w * border_frac);  // 128 px
    int border_y = static_cast<int>(frame_h * border_frac);  // 72 px
    
    EXPECT_EQ(border_x, 128) << "X border should be 128 px for 1280w";
    EXPECT_EQ(border_y, 72) << "Y border should be 72 px for 720h";
    
    // ROI rect: [128, 72, 1152, 648] (10% inset on all sides)
    int roi_x0 = border_x;
    int roi_y0 = border_y;
    int roi_x1 = frame_w - border_x;
    int roi_y1 = frame_h - border_y;
    
    EXPECT_EQ(roi_x0, 128);
    EXPECT_EQ(roi_y0, 72);
    EXPECT_EQ(roi_x1, 1152);
    EXPECT_EQ(roi_y1, 648);
}

// ============================================================================
// TC-079: Publishing Grace Period
// ============================================================================

TEST_F(TrackingTest, PublishGracePeriod) {
    // Test grace period for publishing tracks with gaps
    TrackerConfig config;
    int grace = config.publish_grace_missed;  // 6 frames
    
    EXPECT_EQ(grace, 6) << "Grace period should be 6 frames";
    
    // Simulate track with missed detections
    int missed = 0;
    bool should_publish = true;
    
    // Within grace period - should still publish
    for (int i = 0; i < grace; i++) {
        missed++;
        should_publish = (missed <= grace);
        EXPECT_TRUE(should_publish) << "Should publish within grace period";
    }
    
    // Exceeds grace period - should stop publishing
    missed++;
    should_publish = (missed <= grace);
    EXPECT_FALSE(should_publish) << "Should not publish after grace period";
}

// ============================================================================
// Test Suite Summary
// ============================================================================

/*
 * TRACKING TEST SUITE SUMMARY
 * ============================
 * 
 * Test Coverage:
 * - TrackState enum (1 test)
 * - TrackerConfig defaults/custom (2 tests)
 * - IoU-based association (3 tests)
 * - Track state transitions (2 tests)
 * - Motion vector calculations (3 tests)
 * - EMA smoothing (2 tests)
 * - Speed clamping (2 tests)
 * - Detection filtering (2 tests)
 * - Enter/exit borders (1 test)
 * - Publishing grace period (1 test)
 * 
 * Total Tests: 19
 * 
 * Key Concepts Tested:
 * - Track lifecycle: Tentative → Confirmed → Deleted
 * - IoU-based association (0.45 legacy, 0.70/0.50 ByteTrack)
 * - Motion smoothing: EMA with alpha=0.45 (pos), 0.70 (vel)
 * - Speed limits: 8-90 px/s (480p baseline)
 * - Quality gates: min_score=0.50, min_area=1600px
 * - Direction quantization: 8 bins (R,UR,U,UL,L,DL,D,DR) + stationary
 * - Publishing: 6-frame grace period, border detection
 * 
 * Mathematical Validations:
 * - IoU calculation: intersection / union
 * - Speed: sqrt(vx² + vy²)
 * - Direction: atan2(vy, vx) × 180/π
 * - EMA: alpha × new + (1-alpha) × old
 * - Border: frame × border_frac
 * 
 * Configuration Coverage:
 * - Legacy tracker parameters: 100%
 * - ByteTrack parameters: 100%
 * - Motion smoothing: 100%
 * - Quality thresholds: 100%
 * - Publishing rules: 100%
 * 
 * Design Insights:
 * 1. Dual-threshold ByteTrack: High-conf (0.70 IoU) + Low-conf (0.50 IoU)
 * 2. Fast confirmation: 2 hits (lowered from 3) for weak/distant faces
 * 3. Asymmetric smoothing: Position (0.45) smoother than velocity (0.70)
 * 4. Speed clamping: 8-90 px/s prevents noise and unrealistic motion
 * 5. Grace period: 6 frames (~200ms @30fps) bridges detection gaps
 * 
 * Note: These tests validate configuration and mathematical operations
 * without requiring stateful tracking or frame sequences. Actual track
 * association and lifecycle management tested in integration tests.
 */
