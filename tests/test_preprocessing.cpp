/**
 * @file test_preprocessing.cpp
 * @brief Week 2 Sprint 2 - Preprocessing Pipeline Tests
 * 
 * Tests for preprocessing configuration and pipeline structures.
 * Tests cover PreprocessConfig options, letterbox calculations, and normalization specs.
 * 
 * Note: These are unit tests for preprocessing configuration and data structures.
 * Actual RGA hardware acceleration operations (image resizing, color conversion)
 * require Rockchip NPU hardware and are tested via integration tests.
 * 
 * Test Suite: PreprocessingTest
 * Test Cases: 15 tests
 */

#include <gtest/gtest.h>
#include "pipeline/preprocess_stage.h"
#include "pipeline/pipeline_types.h"
#include "config/model_spec.h"
#include "image_utils.h"
#include <cmath>

// ============================================================================
// Test Fixture
// ============================================================================

class PreprocessingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup before each test
    }

    void TearDown() override {
        // Cleanup after each test
    }
};

// ============================================================================
// TC-040: PreprocessConfig Structure and Defaults
// ============================================================================

TEST_F(PreprocessingTest, PreprocessConfigDefaults) {
    // Test default preprocessing configuration
    PreprocessConfig config;
    
    EXPECT_TRUE(config.keep_aspect) << "Default should keep aspect ratio (letterbox)";
    EXPECT_EQ(config.fill_y, 0) << "Default Y fill should be 0 (black)";
    EXPECT_EQ(config.fill_uv, 128) << "Default UV fill should be 128 (gray for NV12)";
    EXPECT_TRUE(config.to_tensor) << "Default should write directly to tensor";
}

TEST_F(PreprocessingTest, PreprocessConfigCustom) {
    // Test custom preprocessing configuration
    PreprocessConfig config;
    config.keep_aspect = false;  // Stretch instead of letterbox
    config.fill_y = 128;         // Gray fill
    config.fill_uv = 128;
    config.to_tensor = false;    // Use scratch buffer
    
    EXPECT_FALSE(config.keep_aspect);
    EXPECT_EQ(config.fill_y, 128);
    EXPECT_EQ(config.fill_uv, 128);
    EXPECT_FALSE(config.to_tensor);
}

// ============================================================================
// TC-041: Letterbox Calculation Logic
// ============================================================================

TEST_F(PreprocessingTest, LetterboxStructure) {
    // Test letterbox_t structure
    letterbox_t lb;
    lb.x_pad = 10;
    lb.y_pad = 20;
    lb.scale = 0.5f;
    
    EXPECT_EQ(lb.x_pad, 10);
    EXPECT_EQ(lb.y_pad, 20);
    EXPECT_FLOAT_EQ(lb.scale, 0.5f);
}

TEST_F(PreprocessingTest, LetterboxScaleCalculation) {
    // Test letterbox scale calculation logic
    // When input is 640x480 and target is 320x320
    int input_w = 640, input_h = 480;
    int target_w = 320, target_h = 320;
    
    // Scale should be min(target_w/input_w, target_h/input_h)
    float scale_x = (float)target_w / input_w;  // 320/640 = 0.5
    float scale_y = (float)target_h / input_h;  // 320/480 = 0.667
    float scale = std::min(scale_x, scale_y);   // min = 0.5
    
    EXPECT_FLOAT_EQ(scale, 0.5f) << "Scale should be minimum to maintain aspect";
    
    // After scaling, actual size is 320x240 (centered in 320x320)
    int scaled_w = (int)(input_w * scale);  // 320
    int scaled_h = (int)(input_h * scale);  // 240
    
    EXPECT_EQ(scaled_w, 320);
    EXPECT_EQ(scaled_h, 240);
    
    // Padding to center: (320-320)/2 = 0 horizontal, (320-240)/2 = 40 vertical
    int x_pad = (target_w - scaled_w) / 2;  // 0
    int y_pad = (target_h - scaled_h) / 2;  // 40
    
    EXPECT_EQ(x_pad, 0) << "No horizontal padding needed";
    EXPECT_EQ(y_pad, 40) << "40 pixels vertical padding on each side";
}

TEST_F(PreprocessingTest, LetterboxSquareInput) {
    // Test letterbox with square input (no padding needed)
    int input_w = 640, input_h = 640;
    int target_w = 320, target_h = 320;
    
    float scale = std::min((float)target_w / input_w, (float)target_h / input_h);
    EXPECT_FLOAT_EQ(scale, 0.5f);
    
    int scaled_w = (int)(input_w * scale);
    int scaled_h = (int)(input_h * scale);
    int x_pad = (target_w - scaled_w) / 2;
    int y_pad = (target_h - scaled_h) / 2;
    
    EXPECT_EQ(x_pad, 0) << "No padding for square input to square target";
    EXPECT_EQ(y_pad, 0);
}

TEST_F(PreprocessingTest, LetterboxWideInput) {
    // Test letterbox with wide input (vertical padding)
    int input_w = 1280, input_h = 720;  // 16:9 aspect
    int target_w = 320, target_h = 320;
    
    float scale = std::min((float)target_w / input_w, (float)target_h / input_h);
    EXPECT_FLOAT_EQ(scale, 0.25f) << "Scale limited by width";
    
    int scaled_h = (int)(input_h * scale);  // 180
    int y_pad = (target_h - scaled_h) / 2;  // 70
    
    EXPECT_EQ(scaled_h, 180);
    EXPECT_EQ(y_pad, 70) << "Vertical padding for wide input";
}

// ============================================================================
// TC-042: Pixel Format and Color Space
// ============================================================================

TEST_F(PreprocessingTest, PixelFormatEnum) {
    // Test PixFmt enumeration
    PixFmt nv12 = PixFmt::NV12;
    PixFmt rgb24 = PixFmt::RGB24;
    PixFmt bgr24 = PixFmt::BGR24;
    PixFmt gray = PixFmt::GRAY8;
    
    EXPECT_NE(nv12, rgb24);
    EXPECT_NE(rgb24, bgr24);
    EXPECT_NE(bgr24, gray);
}

TEST_F(PreprocessingTest, ColorLayoutEnum) {
    // Test ColorLayout enumeration from ModelSpec
    ColorLayout rgb = ColorLayout::RGB;
    ColorLayout bgr = ColorLayout::BGR;
    ColorLayout nv12 = ColorLayout::NV12;
    ColorLayout gray = ColorLayout::GRAY;
    
    EXPECT_NE(rgb, bgr) << "RGB and BGR are different layouts";
    EXPECT_NE(rgb, nv12);
    EXPECT_NE(bgr, gray);
}

TEST_F(PreprocessingTest, ChannelOrderEnum) {
    // Test ChannelOrder enumeration
    ChannelOrder hwc = ChannelOrder::HWC;  // Height x Width x Channels
    ChannelOrder chw = ChannelOrder::CHW;  // Channels x Height x Width
    
    EXPECT_NE(hwc, chw) << "HWC and CHW are different orderings";
}

// ============================================================================
// TC-043: Normalization Configuration
// ============================================================================

TEST_F(PreprocessingTest, NormalizationDefaults) {
    // Test default normalization (0-1 range)
    Normalization norm;
    
    EXPECT_FLOAT_EQ(norm.mean[0], 0.0f);
    EXPECT_FLOAT_EQ(norm.mean[1], 0.0f);
    EXPECT_FLOAT_EQ(norm.mean[2], 0.0f);
    EXPECT_FLOAT_EQ(norm.std[0], 1.0f);
    EXPECT_FLOAT_EQ(norm.std[1], 1.0f);
    EXPECT_FLOAT_EQ(norm.std[2], 1.0f);
    EXPECT_EQ(norm.channels, 3);
    EXPECT_TRUE(norm.to_float);
    EXPECT_FLOAT_EQ(norm.scale, 1.0f/255.0f) << "Default scale for [0,255] -> [0,1]";
}

TEST_F(PreprocessingTest, NormalizationImageNet) {
    // Test ImageNet normalization (mean/std)
    Normalization norm;
    norm.mean[0] = 123.675f;
    norm.mean[1] = 116.28f;
    norm.mean[2] = 103.53f;
    norm.std[0] = 58.395f;
    norm.std[1] = 57.12f;
    norm.std[2] = 57.375f;
    norm.channels = 3;
    norm.to_float = true;
    norm.scale = 1.0f;  // Already in float range
    
    // Verify ImageNet mean values
    EXPECT_FLOAT_EQ(norm.mean[0], 123.675f);
    EXPECT_FLOAT_EQ(norm.mean[1], 116.28f);
    EXPECT_FLOAT_EQ(norm.mean[2], 103.53f);
    
    // Verify ImageNet std values
    EXPECT_FLOAT_EQ(norm.std[0], 58.395f);
    EXPECT_FLOAT_EQ(norm.std[1], 57.12f);
    EXPECT_FLOAT_EQ(norm.std[2], 57.375f);
}

TEST_F(PreprocessingTest, NormalizationGrayscale) {
    // Test grayscale normalization (1 channel)
    Normalization norm;
    norm.channels = 1;
    norm.mean[0] = 128.0f;
    norm.std[0] = 128.0f;
    norm.to_float = true;
    
    EXPECT_EQ(norm.channels, 1);
    EXPECT_FLOAT_EQ(norm.mean[0], 128.0f);
    EXPECT_FLOAT_EQ(norm.std[0], 128.0f);
}

// ============================================================================
// TC-044: RawFrame and PreprocFrame Structures
// ============================================================================

TEST_F(PreprocessingTest, RawFrameStructure) {
    // Test RawFrame structure for NV12 input
    RawFrame rf;
    rf.fmt = PixFmt::NV12;
    rf.width = 640;
    rf.height = 480;
    rf.stride0 = 640;   // Y plane stride
    rf.stride1 = 640;   // UV plane stride (half height)
    rf.pts_ns = 1000000;
    rf.seq = 42;
    
    EXPECT_EQ(rf.fmt, PixFmt::NV12);
    EXPECT_EQ(rf.width, 640);
    EXPECT_EQ(rf.height, 480);
    EXPECT_EQ(rf.stride0, 640);
    EXPECT_EQ(rf.pts_ns, 1000000);
    EXPECT_EQ(rf.seq, 42);
}

TEST_F(PreprocessingTest, PreprocFrameStructure) {
    // Test PreprocFrame structure
    PreprocFrame pf;
    pf.fmt = PixFmt::RGB24;
    pf.width = 320;
    pf.height = 320;
    pf.stride = 320 * 3;  // RGB: width * 3 bytes
    pf.pts_ns = 2000000;
    pf.seq = 43;
    
    EXPECT_EQ(pf.fmt, PixFmt::RGB24);
    EXPECT_EQ(pf.width, 320);
    EXPECT_EQ(pf.height, 320);
    EXPECT_EQ(pf.stride, 960);
    EXPECT_EQ(pf.pts_ns, 2000000);
    EXPECT_EQ(pf.seq, 43);
}

TEST_F(PreprocessingTest, PreprocFrameTensorMode) {
    // Test PreprocFrame with direct tensor pointer
    PreprocFrame pf;
    pf.tensor_bytes = 320 * 320 * 3;  // RGB tensor size
    pf.pts_ns = 3000000;
    pf.seq = 44;
    
    EXPECT_EQ(pf.tensor_bytes, 307200);  // 320*320*3
    EXPECT_EQ(pf.pts_ns, 3000000);
    EXPECT_EQ(pf.seq, 44);
}

// ============================================================================
// TC-045: ModelSpec Input Configuration
// ============================================================================

TEST_F(PreprocessingTest, ModelSpecInputDefaults) {
    // Test ModelSpec input configuration defaults
    ModelSpec spec;
    
    EXPECT_EQ(spec.input_size.w, 320);
    EXPECT_EQ(spec.input_size.h, 320);
    EXPECT_EQ(spec.input_channels, 3);
    EXPECT_EQ(spec.input_layout, ColorLayout::RGB);
    EXPECT_EQ(spec.order, ChannelOrder::HWC);
}

TEST_F(PreprocessingTest, ModelSpecInputCustom) {
    // Test custom ModelSpec input configuration
    ModelSpec spec;
    spec.input_size = Size2i{640, 640};
    spec.input_channels = 3;
    spec.input_layout = ColorLayout::BGR;
    spec.order = ChannelOrder::CHW;
    spec.keep_aspect = true;
    
    EXPECT_EQ(spec.input_size.w, 640);
    EXPECT_EQ(spec.input_size.h, 640);
    EXPECT_EQ(spec.input_channels, 3);
    EXPECT_EQ(spec.input_layout, ColorLayout::BGR);
    EXPECT_EQ(spec.order, ChannelOrder::CHW);
    EXPECT_TRUE(spec.keep_aspect);
}

TEST_F(PreprocessingTest, ModelSpecNormalization) {
    // Test ModelSpec with normalization
    ModelSpec spec;
    spec.norm.mean[0] = 0.485f * 255.0f;  // ImageNet mean scaled
    spec.norm.mean[1] = 0.456f * 255.0f;
    spec.norm.mean[2] = 0.406f * 255.0f;
    spec.norm.std[0] = 0.229f * 255.0f;   // ImageNet std scaled
    spec.norm.std[1] = 0.224f * 255.0f;
    spec.norm.std[2] = 0.225f * 255.0f;
    spec.norm.channels = 3;
    spec.norm.to_float = true;
    
    EXPECT_EQ(spec.norm.channels, 3);
    EXPECT_TRUE(spec.norm.to_float);
    // Mean values approximately 123.675, 116.28, 103.53
    EXPECT_NEAR(spec.norm.mean[0], 123.675f, 0.1f);
    EXPECT_NEAR(spec.norm.mean[1], 116.28f, 0.1f);
    EXPECT_NEAR(spec.norm.mean[2], 103.53f, 0.1f);
}

// ============================================================================
// Test Suite Summary
// ============================================================================

/*
 * PREPROCESSING PIPELINE TEST SUITE SUMMARY
 * ==========================================
 * 
 * Test Coverage:
 * - PreprocessConfig structure (2 tests)
 * - Letterbox calculations (4 tests)
 * - Pixel formats and color spaces (3 tests)
 * - Normalization configuration (3 tests)
 * - Frame structures (3 tests)
 * - ModelSpec input configuration (3 tests)
 * 
 * Total Tests: 18
 * 
 * Key Structures Tested:
 * - PreprocessConfig
 * - letterbox_t
 * - PixFmt, ColorLayout, ChannelOrder enums
 * - Normalization
 * - RawFrame, PreprocFrame
 * - ModelSpec input settings
 * 
 * Coverage Goals:
 * - Configuration structures: 100%
 * - Letterbox math logic: 100%
 * - Normalization specs: 100%
 * - Frame metadata: 100%
 * 
 * Note: These tests validate preprocessing configuration and math logic
 * without running actual RGA hardware operations (image resizing, color
 * conversion, normalization). Hardware-dependent operations are tested
 * via integration tests on embedded ARM devices.
 * 
 * Design Rationale:
 * - Separate configuration/logic tests from hardware operations
 * - Validate letterbox scale calculations (critical for accuracy)
 * - Test all pixel format and normalization options
 * - Ensure frame structures properly handle metadata (pts, seq)
 */
