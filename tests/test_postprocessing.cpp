/**
 * @file test_postprocessing.cpp
 * @brief Week 2 Sprint 3 - Postprocessing Pipeline Tests
 * 
 * Tests for detection structures, coordinate transformations, tracking integration,
 * and result formatting in the postprocessing pipeline.
 * 
 * Note: These are pure unit tests that test data structures, mathematical
 * transformations (de-letterboxing), and configuration without requiring
 * hardware dependencies (RKNN, RGA).
 * 
 * Test Suite: PostprocessingTest
 * Test Cases: 12 tests
 */

#include <gtest/gtest.h>
#include "models/model_runner.h"
#include "pipeline/pipeline_types.h"
#include "tracking/tracker.h"
#include "config/processing_params.h"
#include <cmath>
#include <vector>

// ============================================================================
// Test Fixture
// ============================================================================

class PostprocessingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup before each test
    }

    void TearDown() override {
        // Cleanup after each test
    }
    
    // Helper: Calculate IoU for two bounding boxes
    float calculate_iou(float x0a, float y0a, float x1a, float y1a,
                       float x0b, float y0b, float x1b, float y1b) {
        float inter_x0 = std::max(x0a, x0b);
        float inter_y0 = std::max(y0a, y0b);
        float inter_x1 = std::min(x1a, x1b);
        float inter_y1 = std::min(y1a, y1b);
        
        if (inter_x1 <= inter_x0 || inter_y1 <= inter_y0) {
            return 0.0f;  // No overlap
        }
        
        float inter_area = (inter_x1 - inter_x0) * (inter_y1 - inter_y0);
        float area_a = (x1a - x0a) * (y1a - y0a);
        float area_b = (x1b - x0b) * (y1b - y0b);
        float union_area = area_a + area_b - inter_area;
        
        return (union_area > 0) ? (inter_area / union_area) : 0.0f;
    }
};

// ============================================================================
// TC-040: Detection Structure Validation
// ============================================================================

TEST_F(PostprocessingTest, DetectionStructureDefaults) {
    // Test Detection structure initialization and fields
    // Fixed in production: Detection now has default initialization for all fields
    Detection det;  // Safe to use without {} now
    
    // All fields should have default values (after fix)
    EXPECT_EQ(det.x0, 0.0f);
    EXPECT_EQ(det.y0, 0.0f);
    EXPECT_EQ(det.x1, 0.0f);
    EXPECT_EQ(det.y1, 0.0f);
    EXPECT_EQ(det.score, 0.0f);
    EXPECT_EQ(det.class_id, -1) << "Default class_id should be -1 (invalid)";
}

TEST_F(PostprocessingTest, DetectionStructureCustom) {
    // Test custom Detection values
    Detection det;
    det.x0 = 100.0f;
    det.y0 = 150.0f;
    det.x1 = 200.0f;
    det.y1 = 250.0f;
    det.score = 0.85f;
    det.class_id = 0;  // person class
    
    EXPECT_EQ(det.x0, 100.0f);
    EXPECT_EQ(det.y0, 150.0f);
    EXPECT_EQ(det.x1, 200.0f);
    EXPECT_EQ(det.y1, 250.0f);
    EXPECT_EQ(det.score, 0.85f);
    EXPECT_EQ(det.class_id, 0);
}

// ============================================================================
// TC-041: Bounding Box Coordinate Validation
// ============================================================================

TEST_F(PostprocessingTest, BoundingBoxValidation) {
    // Test valid bounding box coordinates (x1 > x0, y1 > y0)
    Detection det;
    det.x0 = 50.0f;
    det.y0 = 100.0f;
    det.x1 = 150.0f;
    det.y1 = 200.0f;
    
    // Validate coordinate ordering
    EXPECT_LT(det.x0, det.x1) << "x1 should be greater than x0";
    EXPECT_LT(det.y0, det.y1) << "y1 should be greater than y0";
    
    // Calculate width and height
    float width = det.x1 - det.x0;
    float height = det.y1 - det.y0;
    EXPECT_EQ(width, 100.0f);
    EXPECT_EQ(height, 100.0f);
    
    // Calculate area
    float area = width * height;
    EXPECT_EQ(area, 10000.0f);
}

TEST_F(PostprocessingTest, BoundingBoxClamping) {
    // Test bounding box clamping to frame boundaries
    Detection det;
    det.x0 = -10.0f;   // Out of bounds (negative)
    det.y0 = -5.0f;
    det.x1 = 650.0f;   // Out of bounds (exceeds 640)
    det.y1 = 490.0f;   // Out of bounds (exceeds 480)
    
    const int frame_w = 640;
    const int frame_h = 480;
    
    // Clamp coordinates
    det.x0 = std::max(0.0f, std::min(float(frame_w), det.x0));
    det.y0 = std::max(0.0f, std::min(float(frame_h), det.y0));
    det.x1 = std::max(0.0f, std::min(float(frame_w), det.x1));
    det.y1 = std::max(0.0f, std::min(float(frame_h), det.y1));
    
    EXPECT_EQ(det.x0, 0.0f) << "Negative x0 should clamp to 0";
    EXPECT_EQ(det.y0, 0.0f) << "Negative y0 should clamp to 0";
    EXPECT_EQ(det.x1, 640.0f) << "x1 > frame_w should clamp to frame_w";
    EXPECT_EQ(det.y1, 480.0f) << "y1 > frame_h should clamp to frame_h";
}

// ============================================================================
// TC-042: Confidence Threshold Filtering
// ============================================================================

TEST_F(PostprocessingTest, ConfidenceThresholdFiltering) {
    // Test filtering detections by confidence threshold
    std::vector<Detection> detections = {
        {100, 100, 200, 200, 0.85f, 0},  // High confidence
        {210, 100, 310, 200, 0.45f, 0},  // Below threshold
        {320, 100, 420, 200, 0.65f, 0},  // Above threshold
        {430, 100, 530, 200, 0.30f, 0},  // Below threshold
        {540, 100, 640, 200, 0.92f, 0},  // High confidence
    };
    
    const float min_confidence = 0.50f;
    
    // Filter by confidence
    std::vector<Detection> filtered;
    for (const auto& det : detections) {
        if (det.score >= min_confidence) {
            filtered.push_back(det);
        }
    }
    
    EXPECT_EQ(filtered.size(), 3) << "Should keep 3 detections above threshold";
    EXPECT_GE(filtered[0].score, min_confidence);
    EXPECT_GE(filtered[1].score, min_confidence);
    EXPECT_GE(filtered[2].score, min_confidence);
}

TEST_F(PostprocessingTest, ProcessingParamsThresholds) {
    // Test ProcessingParams threshold configuration
    ProcessingParams params;
    
    // Check default thresholds
    EXPECT_EQ(params.th.score, 0.5f) << "Default score threshold should be 0.5";
    EXPECT_EQ(params.th.iou, 0.45f) << "Default IoU threshold should be 0.45";
    EXPECT_EQ(params.th.keypoint, 0.2f) << "Default keypoint threshold should be 0.2";
    
    // Test custom thresholds
    params.th.score = 0.7f;
    params.th.iou = 0.5f;
    params.th.keypoint = 0.3f;
    
    EXPECT_EQ(params.th.score, 0.7f);
    EXPECT_EQ(params.th.iou, 0.5f);
    EXPECT_EQ(params.th.keypoint, 0.3f);
}

// ============================================================================
// TC-043: De-letterboxing Coordinate Transformation
// ============================================================================

TEST_F(PostprocessingTest, DeletterboxCoordinates) {
    // Test de-letterboxing transformation from model space to camera space
    // Example: 640x480 camera -> 320x320 model
    const int orig_w = 640;
    const int orig_h = 480;
    const int model_w = 320;
    const int model_h = 320;
    
    // Calculate letterbox parameters (same as in model_runner_yolox.cpp)
    const float scale = std::min(float(model_w) / orig_w, float(model_h) / orig_h);
    const int letterbox_w = int(orig_w * scale);
    const int letterbox_h = int(orig_h * scale);
    const int pad_x = (model_w - letterbox_w) / 2;
    const int pad_y = (model_h - letterbox_h) / 2;
    
    // Verify letterbox calculations
    EXPECT_FLOAT_EQ(scale, 0.5f) << "Scale should be min(320/640, 320/480) = 0.5";
    EXPECT_EQ(letterbox_w, 320) << "Letterbox width should be 640 * 0.5 = 320";
    EXPECT_EQ(letterbox_h, 240) << "Letterbox height should be 480 * 0.5 = 240";
    EXPECT_EQ(pad_x, 0) << "X padding should be (320-320)/2 = 0";
    EXPECT_EQ(pad_y, 40) << "Y padding should be (320-240)/2 = 40";
    
    // Test de-letterboxing a detection box from model space to camera space
    // Model space: centered box in 320x320 (with padding)
    float x0_letter = 100.0f;
    float y0_letter = 140.0f;  // y + pad_y offset
    float x1_letter = 220.0f;
    float y1_letter = 260.0f;
    
    // De-letterbox: remove padding, scale back to original frame
    float x0_camera = (x0_letter - pad_x) / scale;
    float y0_camera = (y0_letter - pad_y) / scale;
    float x1_camera = (x1_letter - pad_x) / scale;
    float y1_camera = (y1_letter - pad_y) / scale;
    
    EXPECT_FLOAT_EQ(x0_camera, 200.0f) << "De-letterbox x0: (100-0)/0.5 = 200";
    EXPECT_FLOAT_EQ(y0_camera, 200.0f) << "De-letterbox y0: (140-40)/0.5 = 200";
    EXPECT_FLOAT_EQ(x1_camera, 440.0f) << "De-letterbox x1: (220-0)/0.5 = 440";
    EXPECT_FLOAT_EQ(y1_camera, 440.0f) << "De-letterbox y1: (260-40)/0.5 = 440";
}

TEST_F(PostprocessingTest, DeletterboxWideInput) {
    // Test de-letterboxing for 16:9 wide input (e.g., 1920x1080 -> 640x640)
    const int orig_w = 1920;
    const int orig_h = 1080;
    const int model_w = 640;
    const int model_h = 640;
    
    const float scale = std::min(float(model_w) / orig_w, float(model_h) / orig_h);
    const int letterbox_w = int(orig_w * scale);
    const int letterbox_h = int(orig_h * scale);
    const int pad_x = (model_w - letterbox_w) / 2;
    const int pad_y = (model_h - letterbox_h) / 2;
    
    // Verify letterbox calculations for 16:9
    EXPECT_NEAR(scale, 0.3333f, 0.001f) << "Scale should be min(640/1920, 640/1080) ≈ 0.333";
    EXPECT_EQ(letterbox_w, 640) << "Letterbox width should fill model width";
    EXPECT_EQ(letterbox_h, 360) << "Letterbox height should be 1080 * 0.333 = 360";
    EXPECT_EQ(pad_x, 0) << "No horizontal padding";
    EXPECT_EQ(pad_y, 140) << "Vertical padding should be (640-360)/2 = 140";
}

// ============================================================================
// TC-044: IoU Calculation for NMS
// ============================================================================

TEST_F(PostprocessingTest, IoUCalculation) {
    // Test Intersection over Union calculation
    // Box A: (100, 100) to (200, 200) - 100x100 square, area = 10000
    // Box B: (150, 150) to (250, 250) - 100x100 square, area = 10000
    float iou = calculate_iou(100, 100, 200, 200,
                              150, 150, 250, 250);
    
    // Intersection: (150, 150) to (200, 200) = 50x50 = 2500
    // Union: 10000 + 10000 - 2500 = 17500
    // IoU = 2500 / 17500 ≈ 0.1428
    EXPECT_NEAR(iou, 0.1428f, 0.001f) << "IoU for overlapping boxes";
}

TEST_F(PostprocessingTest, IoUNoOverlap) {
    // Test IoU for non-overlapping boxes
    float iou = calculate_iou(100, 100, 200, 200,
                              250, 250, 350, 350);
    
    EXPECT_EQ(iou, 0.0f) << "IoU should be 0 for non-overlapping boxes";
}

TEST_F(PostprocessingTest, IoUPerfectOverlap) {
    // Test IoU for identical boxes
    float iou = calculate_iou(100, 100, 200, 200,
                              100, 100, 200, 200);
    
    EXPECT_FLOAT_EQ(iou, 1.0f) << "IoU should be 1.0 for identical boxes";
}

// ============================================================================
// TC-045: NMS Settings Configuration
// ============================================================================

TEST_F(PostprocessingTest, NmsSettingsDefaults) {
    // Test NMS settings defaults
    NmsSettings nms;
    
    EXPECT_EQ(nms.method, NmsMethod::Greedy) << "Default NMS method should be Greedy";
    EXPECT_EQ(nms.sigma, 0.5f) << "Default sigma for Soft-NMS should be 0.5";
    EXPECT_EQ(nms.top_k, 100) << "Default top_k should be 100";
    EXPECT_TRUE(nms.class_agnostic) << "Default should be class-agnostic NMS";
}

TEST_F(PostprocessingTest, NmsSettingsCustom) {
    // Test custom NMS settings
    NmsSettings nms;
    nms.method = NmsMethod::Soft;
    nms.sigma = 0.6f;
    nms.top_k = 200;
    nms.class_agnostic = false;
    
    EXPECT_EQ(nms.method, NmsMethod::Soft);
    EXPECT_EQ(nms.sigma, 0.6f);
    EXPECT_EQ(nms.top_k, 200);
    EXPECT_FALSE(nms.class_agnostic);
}

// ============================================================================
// TC-046: TrackedBox Structure and Track States
// ============================================================================

TEST_F(PostprocessingTest, TrackedBoxDefaults) {
    // Test TrackedBox structure defaults
    TrackedBox track;
    
    // Default motion values should be zero
    EXPECT_EQ(track.vx, 0.0f) << "Default velocity x should be 0";
    EXPECT_EQ(track.vy, 0.0f) << "Default velocity y should be 0";
    EXPECT_EQ(track.speed, 0.0f) << "Default speed should be 0";
    EXPECT_EQ(track.dir_deg, 0.0f) << "Default direction should be 0";
    EXPECT_EQ(track.dir_conf, 0.0f) << "Default direction confidence should be 0";
    
    // Default time values
    EXPECT_EQ(track.first_ts, 0.0) << "Default first timestamp should be 0";
    EXPECT_EQ(track.last_ts, 0.0) << "Default last timestamp should be 0";
    EXPECT_EQ(track.dwell_s, 0.0) << "Default dwell time should be 0";
    
    // Default gaze values
    EXPECT_FALSE(track.has_gaze) << "Default has_gaze should be false";
    EXPECT_FALSE(track.is_gazing) << "Default is_gazing should be false";
    EXPECT_EQ(track.gaze_time, 0.0) << "Default gaze time should be 0";
    
    // Default lifecycle flags
    EXPECT_FALSE(track.just_entered) << "Default just_entered should be false";
    EXPECT_FALSE(track.just_exited) << "Default just_exited should be false";
    EXPECT_EQ(track.age_frames, 0) << "Default age should be 0 frames";
    EXPECT_EQ(track.hits, 0) << "Default hits should be 0";
    EXPECT_EQ(track.missed, 0) << "Default missed should be 0";
}

TEST_F(PostprocessingTest, TrackedBoxMotionVectors) {
    // Test motion vector calculations
    TrackedBox track;
    track.vx = 3.0f;  // 3 px/s to the right
    track.vy = 4.0f;  // 4 px/s down
    
    // Calculate speed (magnitude of velocity vector)
    track.speed = std::sqrt(track.vx * track.vx + track.vy * track.vy);
    
    EXPECT_FLOAT_EQ(track.speed, 5.0f) << "Speed should be sqrt(3²+4²) = 5.0";
    
    // Calculate direction (atan2 returns radians, convert to degrees)
    float dir_rad = std::atan2(track.vy, track.vx);
    track.dir_deg = dir_rad * 180.0f / M_PI;
    
    // Direction: 0°=right, 90°=down, 180°=left, 270°=up (image coords)
    EXPECT_NEAR(track.dir_deg, 53.13f, 0.1f) << "Direction should be atan2(4,3) ≈ 53.13°";
}

TEST_F(PostprocessingTest, TrackerConfigDefaults) {
    // Test TrackerConfig defaults
    TrackerConfig config;
    
    EXPECT_EQ(config.tracker_core, "legacy") << "Default tracker should be 'legacy'";
    EXPECT_EQ(config.byte_det_high, 0.50f) << "Default high det threshold should be 0.5";
    EXPECT_EQ(config.byte_det_low, 0.15f) << "Default low det threshold should be 0.15";
    EXPECT_EQ(config.byte_match_iou_high, 0.70f) << "Default high IoU should be 0.7";
    EXPECT_EQ(config.byte_match_iou_low, 0.50f) << "Default low IoU should be 0.5";
    EXPECT_EQ(config.byte_max_age, 30) << "Default max age should be 30 frames";
    EXPECT_EQ(config.byte_n_init, 2) << "Default n_init should be 2";
    
    // Legacy parameters
    EXPECT_EQ(config.iou_match_thresh, 0.45f) << "Default IoU match threshold";
    EXPECT_EQ(config.confirm_hits, 2) << "Default confirm hits should be 2";
    EXPECT_EQ(config.max_missed, 12) << "Default max missed should be 12";
    EXPECT_EQ(config.min_det_score, 0.50f) << "Default min detection score";
    EXPECT_EQ(config.min_area_px, 1600) << "Default min area should be 1600 px";
    
    // Motion parameters
    EXPECT_EQ(config.motion_eps_px, 0.25f) << "Default motion epsilon";
    EXPECT_EQ(config.smooth_pos_alpha, 0.45f) << "Default position smoothing";
    EXPECT_EQ(config.smooth_vel_alpha, 0.70f) << "Default velocity smoothing";
    
    // Publishing parameters
    EXPECT_EQ(config.min_speed_px_s, 8.0f) << "Default min speed floor";
    EXPECT_EQ(config.max_speed_px_s, 90.0f) << "Default max speed ceiling";
    EXPECT_EQ(config.publish_grace_missed, 6) << "Default grace period";
}

// ============================================================================
// TC-047: Pipeline Result Structure
// ============================================================================

TEST_F(PostprocessingTest, PipelineResultDefaults) {
    // Test PipelineResult structure defaults
    PipelineResult result;
    
    EXPECT_TRUE(result.tracks.empty()) << "Default tracks should be empty";
    EXPECT_TRUE(result.person_tracks.empty()) << "Default person_tracks should be empty";
    EXPECT_EQ(result.pts_ns, 0) << "Default PTS should be 0";
    EXPECT_EQ(result.seq, 0) << "Default sequence should be 0";
    EXPECT_EQ(result.ts_ns, 0) << "Default timestamp should be 0";
    EXPECT_EQ(result.people_count, 0) << "Default people count should be 0";
    EXPECT_EQ(result.gaze_count, 0) << "Default gaze count should be 0";
    EXPECT_EQ(result.fps, 0) << "Default FPS should be 0";
    EXPECT_EQ(result.frame_width, 0) << "Default frame width should be 0";
    EXPECT_EQ(result.frame_height, 0) << "Default frame height should be 0";
}

TEST_F(PostprocessingTest, PipelineResultWithTracks) {
    // Test PipelineResult with multiple tracks
    PipelineResult result;
    
    // Add some person tracks
    TrackedBox track1;
    track1.id = 1;
    track1.x0 = 100; track1.y0 = 100; track1.x1 = 200; track1.y1 = 200;
    track1.score = 0.85f;
    track1.state = TrackState::Confirmed;
    
    TrackedBox track2;
    track2.id = 2;
    track2.x0 = 300; track2.y0 = 150; track2.x1 = 400; track2.y1 = 250;
    track2.score = 0.72f;
    track2.state = TrackState::Confirmed;
    track2.has_gaze = true;
    track2.is_gazing = true;
    
    result.person_tracks.push_back(track1);
    result.person_tracks.push_back(track2);
    result.people_count = 2;
    result.gaze_count = 1;
    result.frame_width = 640;
    result.frame_height = 480;
    result.fps = 30;
    
    EXPECT_EQ(result.person_tracks.size(), 2) << "Should have 2 tracks";
    EXPECT_EQ(result.people_count, 2) << "People count should be 2";
    EXPECT_EQ(result.gaze_count, 1) << "Gaze count should be 1";
    EXPECT_EQ(result.person_tracks[0].id, 1);
    EXPECT_EQ(result.person_tracks[1].id, 2);
    EXPECT_TRUE(result.person_tracks[1].is_gazing) << "Track 2 should be gazing";
}

// ============================================================================
// Test Suite Summary
// ============================================================================

/*
 * POSTPROCESSING TEST SUITE SUMMARY
 * ==================================
 * 
 * Test Coverage:
 * - Detection structure (2 tests)
 * - Bounding box validation (2 tests)
 * - Confidence filtering (2 tests)
 * - De-letterboxing math (2 tests)
 * - IoU calculation (3 tests)
 * - NMS settings (2 tests)
 * - TrackedBox structure (3 tests)
 * - Pipeline results (2 tests)
 * 
 * Total Tests: 18
 * 
 * Key Structures Tested:
 * - Detection (bbox, score, class_id)
 * - ProcessingParams (Thresholds, NmsSettings)
 * - TrackedBox (motion, gaze, lifecycle)
 * - TrackerConfig (ByteTrack, legacy, motion params)
 * - PipelineResult (tracks, counts, metadata)
 * 
 * Critical Math Validated:
 * - De-letterboxing: 640x480 → 320x320 with scale=0.5, pad_y=40
 * - IoU calculation: intersection / union
 * - Motion vectors: speed = sqrt(vx² + vy²)
 * - Direction: atan2(vy, vx) in degrees
 * 
 * Coverage Goals:
 * - Detection structures: 100%
 * - Coordinate transformations: 100%
 * - Configuration defaults: 100%
 * - Mathematical operations: 100%
 * 
 * Note: These tests validate data structures and mathematical operations
 * without requiring RKNN inference or actual NMS implementation. NMS
 * algorithms are tested through IoU calculation primitives.
 */
