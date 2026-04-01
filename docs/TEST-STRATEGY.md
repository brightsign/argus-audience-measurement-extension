# Test Strategy - BrightSign NPU Gaze Extension

**Project**: Argus Detection (NPU Gaze + People Counting)  
**Version**: v7.1  
**Date**: 2025-12-11  
**Scope**: RetinaFace (gaze proxy) + YOLOX (people detection) with tracking, zone logic, and MQTT analytics

---

## Executive Summary

This document defines the testing strategy for the BrightSign NPU-based gaze and people counting extension. The system uses:

- **YOLOX** for people detection → tracker (ID persistence) → zone logic (ROI/edge) → events (enter/exit/dwell/direction/speed)
- **RetinaFace** for face detection → gaze estimation and fixation
- **MQTT** `bs/argus/analytics/v7.0` schema for event publication
- **Optional OSD overlay** for visualization

### Key Performance Indicators (KPIs)

| Metric | Target | Critical Threshold |
|--------|--------|-------------------|
| Enter/Exit Event F1 | ≥ 0.97 | ≥ 0.95 |
| Tracking IDF1 | ≥ 0.85 | ≥ 0.80 |
| ID Switches | ≤ 12/hour | ≤ 20/hour |
| Gaze Precision | ≥ 0.90 | ≥ 0.85 |
| Gaze Recall | ≥ 0.85 | ≥ 0.80 |
| Average FPS | ≥ 20 | ≥ 15 |
| E2E Latency (P95) | ≤ 180ms | ≤ 250ms |
| False Events | ≤ 6/hour | ≤ 10/hour |
| Memory Growth | < 20MB/hour | < 50MB/hour |

---

## Table of Contents

1. [Test Pyramid](#1-test-pyramid)
2. [Unit Tests](#2-unit-tests)
3. [Component Tests](#3-component-tests)
4. [System / E2E Tests](#4-system-e2e-tests)
5. [Soak / Reliability Tests](#5-soak-reliability-tests)
6. [Edge Cases](#6-edge-cases)
7. [Ground Truth Requirements](#7-ground-truth-requirements)
8. [Test Tooling](#8-test-tooling)
9. [Debug Playbook](#9-debug-playbook)
10. [Automation](#10-automation)
11. [Release Criteria](#11-release-criteria)
12. [Test Schedule](#12-test-schedule)
13. [Resources](#13-resources)
14. [CI/CD Integration](#14-cicd-integration)

---

## 1. Test Pyramid

```md
                   /\
                  /  \
                 /E2E \        8-12 hours: Soak, reliability, real scenarios
                /------\       
               /        \
              / System  \      4-6 hours: Live camera, scripted scenarios
             /  Tests    \     
            /------------\
           /              \
          /  Component    \    2-3 hours: Model + tracker on recorded clips
         /     Tests       \   
        /------------------\
       /                    \
      /     Unit Tests       \  <1 hour: Logic, math, deterministic
     /                        \ 
    /________________________\
```

### Test Scope

**In Scope:**

- YOLOX people detection accuracy
- RetinaFace face detection accuracy
- Tracker ID persistence and stability
- Zone entry/exit event generation
- Dwell time calculation
- Direction and speed estimation
- Gaze fixation detection
- MQTT message format and timing
- FPS and latency performance
- Memory stability and leak detection
- Error recovery (camera disconnect, MQTT broker disconnect)
- Config auto-reload functionality

**Out of Scope:**

- RTSP server reliability (separate component)
- Network infrastructure testing
- BrightSign OS-level testing
- Display hardware testing
- Third-party library bugs (OpenCV, RKNN)

---

## 2. Unit Tests (Logic & Math - Fast, Deterministic)

**Goal**: Validate core algorithms without hardware dependencies  
**Runtime**: < 1 hour  
**Framework**: Google Test (C++) or pytest (Python wrapper)

### 2.1 Zone Crossing / Enter-Exit FSM

**Test Cases:**

```cpp
TEST(ZoneLogic, SimpleEntry) {
    ROI roi = {192, 144, 448, 336};  // x, y, w, h
    TrackState track;
    
    // Frame 1-5: Outside ROI
    for (int i = 0; i < 5; i++) {
        Point center(100, 200);  // Outside
        auto events = zone_logic.update(track, center, roi);
        EXPECT_FALSE(events.enter);
        EXPECT_FALSE(events.exit);
    }
    
    // Frame 6-10: Cross boundary (with hysteresis)
    for (int i = 0; i < 5; i++) {
        Point center(300, 240);  // Inside
        auto events = zone_logic.update(track, center, roi);
        if (i < 4) {
            EXPECT_FALSE(events.enter);  // Debounce period
        } else {
            EXPECT_TRUE(events.enter);   // Enter fired on 5th frame inside
        }
    }
    
    // Frame 11+: Still inside, no duplicate enter
    auto events = zone_logic.update(track, Point(300, 240), roi);
    EXPECT_FALSE(events.enter);
}

TEST(ZoneLogic, BorderHysteresis) {
    ROI roi = {100, 100, 200, 200};
    int margin = 8;  // pixels
    TrackState track;
    
    // Oscillate ±4 pixels at border
    for (int i = 0; i < 20; i++) {
        int x = 200 + (i % 2 ? 4 : -4);  // Oscillates around right edge
        auto events = zone_logic.update(track, Point(x, 150), roi, margin);
        
        // Should NOT fire multiple enter/exit due to margin
        int enter_count = std::count_if(events.begin(), events.end(), 
                                        [](auto& e) { return e.enter; });
        EXPECT_LE(enter_count, 1);  // At most one enter
    }
}

TEST(ZoneLogic, DebounceWindow) {
    ROI roi = {100, 100, 200, 200};
    int debounce_frames = 5;
    TrackState track;
    
    // Enter ROI
    zone_logic.update(track, Point(150, 150), roi);
    
    // Exit quickly (< debounce frames)
    for (int i = 0; i < 3; i++) {
        auto events = zone_logic.update(track, Point(50, 150), roi);
        EXPECT_FALSE(events.exit);  // Too early to fire exit
    }
    
    // Wait full debounce period
    for (int i = 0; i < debounce_frames; i++) {
        auto events = zone_logic.update(track, Point(50, 150), roi);
    }
    EXPECT_TRUE(events.back().exit);  // Now exit fires
}
```

**Key Test Scenarios:**

- ✅ Single person enters (one-shot `enter=true`)
- ✅ Single person exits (one-shot `exit=true`)
- ✅ Hysteresis margin (4-8px) prevents false events near border
- ✅ Debounce window (5-8 frames) prevents rapid enter/exit toggling
- ✅ No duplicate enter/exit on same track ID
- ✅ Track ID reuse doesn't trigger duplicate events (TTL test)

### 2.2 Direction Calculation

```cpp
TEST(DirectionLogic, FourDirections) {
    std::vector<Point> path_left_to_right = {
        {100, 200}, {150, 200}, {200, 200}, {250, 200}
    };
    Direction dir = calculate_direction(path_left_to_right);
    EXPECT_EQ(dir, Direction::RIGHT);
    
    std::vector<Point> path_top_to_bottom = {
        {200, 100}, {200, 150}, {200, 200}, {200, 250}
    };
    dir = calculate_direction(path_top_to_bottom);
    EXPECT_EQ(dir, Direction::DOWN);
}

TEST(DirectionLogic, VelocityLowPass) {
    // Test that single-frame jitter doesn't flip direction
    std::vector<Point> noisy_path = {
        {100, 200}, {150, 200}, {140, 205}, {200, 200}, {250, 200}
    };
    Direction dir = calculate_direction(noisy_path, /* low_pass */ true);
    EXPECT_EQ(dir, Direction::RIGHT);  // Dominant direction despite noise
}
```

### 2.3 Dwell Time

```cpp
TEST(DwellLogic, MonotonicIncrease) {
    TrackState track;
    ROI roi = {100, 100, 200, 200};
    
    // Enter ROI at frame 10
    for (int frame = 0; frame < 10; frame++) {
        zone_logic.update(track, Point(50, 150), roi, frame);
    }
    zone_logic.update(track, Point(150, 150), roi, 10);  // Enter
    
    // Stay inside for 30 frames
    for (int frame = 11; frame < 40; frame++) {
        auto events = zone_logic.update(track, Point(150, 150), roi, frame);
        EXPECT_GE(events.back().dwell_ms, 0);
        EXPECT_LE(events.back().dwell_ms, (frame - 10) * 33);  // ~30fps
    }
    
    // Exit ROI - dwell should reset
    zone_logic.update(track, Point(50, 150), roi, 50);
    auto events = zone_logic.update(track, Point(50, 150), roi, 51);
    EXPECT_EQ(events.back().dwell_ms, 0);
}
```

### 2.4 Speed Calculation

```cpp
TEST(SpeedLogic, PixelsToRealUnits) {
    // Test pixel-based speed
    std::vector<Point> path = {{100, 200}, {200, 200}};  // 100px horizontal
    double time_ms = 1000.0;  // 1 second
    
    double speed = calculate_speed(path, time_ms);
    EXPECT_NEAR(speed, 100.0, 1.0);  // 100 px/s
}

TEST(SpeedLogic, OutlierClamping) {
    std::vector<Point> path = {
        {100, 200}, {105, 200}, {500, 200}  // Sudden jump (likely error)
    };
    double time_ms = 100.0;
    
    double speed = calculate_speed(path, time_ms, /* clamp */ true);
    EXPECT_LT(speed, 1000.0);  // Should clamp unrealistic speeds
}
```

### 2.5 Tracker Merging/Splitting

```cpp
TEST(TrackerLogic, IoUMerging) {
    Detection det1 = {{100, 100, 50, 100}, 0.9, "person"};
    Detection det2 = {{120, 110, 50, 100}, 0.85, "person"};
    
    double iou = calculate_iou(det1.bbox, det2.bbox);
    EXPECT_GT(iou, 0.5);  // High overlap
    
    bool should_merge = tracker.should_merge(det1, det2, 0.5);
    EXPECT_TRUE(should_merge);
}

TEST(TrackerLogic, IDStability) {
    // Track with short occlusion should maintain ID
    Tracker tracker;
    Detection det1 = {{100, 100, 50, 100}, 0.9, "person"};
    
    // Frame 1: Initial detection
    int id1 = tracker.update({det1}, 1);
    
    // Frames 2-5: Occluded (no detection)
    for (int f = 2; f <= 5; f++) {
        tracker.update({}, f);
    }
    
    // Frame 6: Re-appears nearby
    Detection det2 = {{105, 105, 50, 100}, 0.9, "person"};
    int id2 = tracker.update({det2}, 6);
    
    EXPECT_EQ(id1, id2);  // Should maintain same ID
}

TEST(TrackerLogic, NoIDReuse) {
    Tracker tracker;
    int ttl_frames = 30;
    
    // Track 1: Exists frames 1-10, then exits
    tracker.update({{{100, 100, 50, 100}, 0.9, "person"}}, 1);
    int id1 = tracker.get_tracks()[0].id;
    tracker.update({}, 10);  // Lost
    
    // Frames 11-20: Empty
    for (int f = 11; f <= 20; f++) {
        tracker.update({}, f);
    }
    
    // Frame 21: New person appears (within TTL)
    tracker.update({{{500, 500, 50, 100}, 0.9, "person"}}, 21);
    int id2 = tracker.get_tracks()[0].id;
    
    EXPECT_NE(id1, id2);  // Should NOT reuse ID within TTL
}
```

### 2.6 Gaze Decision Logic

```cpp
TEST(GazeLogic, FaceToPersonAssociation) {
    Detection person = {{100, 100, 80, 200}, 0.9, "person"};
    Detection face1 = {{110, 110, 40, 50}, 0.95, "face"};  // Inside person bbox
    Detection face2 = {{500, 500, 40, 50}, 0.95, "face"};  // Far away
    
    auto face = associate_face_to_person(person, {face1, face2});
    EXPECT_EQ(face.bbox.x, 110);
}

TEST(GazeLogic, FixationRule) {
    GazeTracker gaze_tracker;
    int fixation_frames = 15;  // ~500ms at 30fps
    float confidence_threshold = 0.7;
    
    // Frames 1-10: Low confidence gaze
    for (int i = 0; i < 10; i++) {
        bool gaze = gaze_tracker.update(0.6, i);
        EXPECT_FALSE(gaze);  // Below threshold
    }
    
    // Frames 11-25: High confidence gaze
    for (int i = 11; i < 26; i++) {
        bool gaze = gaze_tracker.update(0.85, i);
        if (i < 11 + fixation_frames) {
            EXPECT_FALSE(gaze);  // Not yet fixated
        } else {
            EXPECT_TRUE(gaze);   // Fixation confirmed
        }
    }
    
    // Frame 26: Drop below threshold
    bool gaze = gaze_tracker.update(0.5, 26);
    EXPECT_FALSE(gaze);  // Immediate reset
}
```

---

## 2.7 Running Unit Tests - Implementation Guide

**Goal**: Set up and execute C++ unit tests using Google Test framework  
**Time**: 1-2 hours initial setup, < 5 minutes per run thereafter

### Project Structure for Unit Tests

```ini
brightsign-npu-gaze-extension-ng/
├── src/
│   ├── attention.cpp          # Main logic
│   ├── zone_logic.cpp         # Zone crossing detection
│   ├── tracker.cpp            # Object tracking
│   ├── gaze_logic.cpp         # Gaze estimation
│   └── ...
├── include/
│   ├── attention.h
│   ├── zone_logic.h
│   ├── tracker.h
│   └── ...
├── tests/                      # NEW: Unit tests directory
│   ├── CMakeLists.txt
│   ├── test_zone_logic.cpp
│   ├── test_tracker.cpp
│   ├── test_gaze_logic.cpp
│   ├── test_direction.cpp
│   ├── test_dwell.cpp
│   └── test_main.cpp          # Google Test main
├── CMakeLists.txt             # Root CMake (updated)
└── build_ls5/
    └── tests/                 # Compiled test binaries
        └── run_all_tests
```

### Step 1: Install Google Test

**Option A: System-wide installation (Recommended for dev machine)**

```bash
# On Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y libgtest-dev cmake

# Build and install Google Test
cd /usr/src/gtest
sudo cmake CMakeLists.txt
sudo make
sudo cp lib/*.a /usr/lib

# Alternatively, use package manager
sudo apt-get install -y googletest
```

**Option B: As a submodule (Recommended for project)**

```bash
cd /path/to/brightsign-npu-gaze-extension-ng

# Add Google Test as submodule
git submodule add https://github.com/google/googletest.git external/googletest
git submodule update --init --recursive
```

### Step 2: Update Root CMakeLists.txt

Add the following to your root `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.10)
project(ArgusExtension)

# ... existing configuration ...

# Google Test setup
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    enable_testing()
    
    # Option A: Use system Google Test
    find_package(GTest REQUIRED)
    include_directories(${GTEST_INCLUDE_DIRS})
    
    # Option B: Use submodule Google Test
    # add_subdirectory(external/googletest)
    # include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})
    
    add_subdirectory(tests)
endif()
```

### Step 3: Create tests/CMakeLists.txt

```cmake
# tests/CMakeLists.txt

# Include parent directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src)

# Collect test source files
set(TEST_SOURCES
    test_main.cpp
    test_zone_logic.cpp
    test_tracker.cpp
    test_gaze_logic.cpp
    test_direction.cpp
    test_dwell.cpp
)

# Collect source files to test (exclude main.cpp)
set(SOURCE_FILES
    ${CMAKE_SOURCE_DIR}/src/zone_logic.cpp
    ${CMAKE_SOURCE_DIR}/src/tracker.cpp
    ${CMAKE_SOURCE_DIR}/src/gaze_logic.cpp
    ${CMAKE_SOURCE_DIR}/src/direction.cpp
    ${CMAKE_SOURCE_DIR}/src/dwell.cpp
    # Add other source files as needed, but exclude main.cpp
)

# Create test executable
add_executable(run_all_tests ${TEST_SOURCES} ${SOURCE_FILES})

# Link Google Test
target_link_libraries(run_all_tests
    GTest::GTest
    GTest::Main
    pthread
    # Add other libraries your code depends on:
    # opencv_core opencv_imgproc ...
)

# Register tests with CTest
add_test(NAME AllUnitTests COMMAND run_all_tests)

# Optional: Add individual test suites
gtest_discover_tests(run_all_tests)
```

### Step 4: Create test_main.cpp

```cpp
// tests/test_main.cpp
#include <gtest/gtest.h>

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

### Step 5: Create Test Files

__Example: tests/test_zone_logic.cpp__

```cpp
#include <gtest/gtest.h>
#include "zone_logic.h"

class ZoneLogicTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize test fixtures
        roi = {192, 144, 448, 336};
    }

    void TearDown() override {
        // Cleanup
    }

    ROI roi;
    ZoneLogic zone_logic;
};

TEST_F(ZoneLogicTest, SimpleEntry) {
    TrackState track;
    
    // Frame 1-5: Outside ROI
    for (int i = 0; i < 5; i++) {
        Point center(100, 200);  // Outside
        auto events = zone_logic.update(track, center, roi);
        EXPECT_FALSE(events.enter) << "Frame " << i << " should not trigger enter";
    }
    
    // Frame 6-9: Inside but debouncing
    for (int i = 0; i < 4; i++) {
        Point center(300, 240);  // Inside
        auto events = zone_logic.update(track, center, roi);
        EXPECT_FALSE(events.enter) << "Debounce period, no enter yet";
    }
    
    // Frame 10: Enter fires
    Point center(300, 240);
    auto events = zone_logic.update(track, center, roi);
    EXPECT_TRUE(events.enter) << "Enter should fire after debounce";
}

TEST_F(ZoneLogicTest, BorderHysteresis) {
    TrackState track;
    int margin = 8;
    
    // Oscillate at border
    int enter_count = 0;
    for (int i = 0; i < 20; i++) {
        int x = 200 + (i % 2 ? 4 : -4);  // ±4px oscillation
        auto events = zone_logic.update(track, Point(x, 150), roi, margin);
        if (events.enter) enter_count++;
    }
    
    EXPECT_LE(enter_count, 1) << "Hysteresis should prevent multiple enters";
}

// Add more tests...
```

### Step 6: Build Tests

```bash
cd /path/to/brightsign-npu-gaze-extension-ng

# Create build directory for tests
mkdir -p build_tests
cd build_tests

# Configure CMake with tests enabled
cmake .. -DBUILD_TESTS=ON

# Build tests
make -j4

# Alternatively, build only tests
make run_all_tests
```

### Step 7: Run Tests

**Run all tests:**

```bash
# From build_tests directory
./tests/run_all_tests

# Or use CTest
ctest --verbose

# Run with color output
./tests/run_all_tests --gtest_color=yes
```

**Run specific test suite:**

```bash
# Run only ZoneLogic tests
./tests/run_all_tests --gtest_filter=ZoneLogic*

# Run only tracker tests
./tests/run_all_tests --gtest_filter=TrackerLogic*

# Run specific test
./tests/run_all_tests --gtest_filter=ZoneLogicTest.SimpleEntry
```

**Generate XML report:**

```bash
./tests/run_all_tests --gtest_output=xml:test_results.xml
```

**Repeat tests for reliability:**

```bash
# Repeat each test 10 times
./tests/run_all_tests --gtest_repeat=10

# Shuffle test order
./tests/run_all_tests --gtest_shuffle
```

### Expected Output

**Successful run:**

```md
[==========] Running 25 tests from 6 test suites.
[----------] Global test environment set-up.
[----------] 5 tests from ZoneLogicTest
[ RUN      ] ZoneLogicTest.SimpleEntry
[       OK ] ZoneLogicTest.SimpleEntry (2 ms)
[ RUN      ] ZoneLogicTest.SimpleExit
[       OK ] ZoneLogicTest.SimpleExit (1 ms)
[ RUN      ] ZoneLogicTest.BorderHysteresis
[       OK ] ZoneLogicTest.BorderHysteresis (3 ms)
[ RUN      ] ZoneLogicTest.DebounceWindow
[       OK ] ZoneLogicTest.DebounceWindow (2 ms)
[ RUN      ] ZoneLogicTest.NoIDReuse
[       OK ] ZoneLogicTest.NoIDReuse (1 ms)
[----------] 5 tests from ZoneLogicTest (9 ms total)

[----------] 4 tests from TrackerLogicTest
[ RUN      ] TrackerLogicTest.IDStability
[       OK ] TrackerLogicTest.IDStability (2 ms)
[ RUN      ] TrackerLogicTest.IoUMerging
[       OK ] TrackerLogicTest.IoUMerging (1 ms)
...
[----------] Global test environment tear-down
[==========] 25 tests from 6 test suites ran. (156 ms total)
[  PASSED  ] 25 tests.
```

**Failed test:**

```ini
[ RUN      ] ZoneLogicTest.BorderHysteresis
/path/to/test_zone_logic.cpp:45: Failure
Expected: (enter_count) <= (1), actual: 3 vs 1
Hysteresis should prevent multiple enters
[  FAILED  ] ZoneLogicTest.BorderHysteresis (4 ms)
```

### Step 8: Add to Makefile (Optional)

Update your `Makefile` to include test targets:

```makefile
# Makefile

.PHONY: test test-build test-run test-clean

# Build and run tests
test: test-build test-run

# Build tests only
test-build:
	@echo "Building unit tests..."
	mkdir -p build_tests
	cd build_tests && cmake .. -DBUILD_TESTS=ON && make -j4

# Run tests only
test-run:
	@echo "Running unit tests..."
	cd build_tests && ctest --output-on-failure

# Clean test build
test-clean:
	rm -rf build_tests

# Run specific test suite
test-zone:
	./build_tests/tests/run_all_tests --gtest_filter=ZoneLogic*

test-tracker:
	./build_tests/tests/run_all_tests --gtest_filter=TrackerLogic*

test-gaze:
	./build_tests/tests/run_all_tests --gtest_filter=GazeLogic*
```

**Usage:**

```bash
# Build and run all tests
make test

# Run only zone logic tests
make test-zone

# Clean and rebuild
make test-clean test
```

### Step 9: Manual Testing Workflow (Run Locally First)

**For immediate local testing before CI/CD setup:**

#### Quick Start (First Time)

```bash
# 1. Navigate to project
cd /home/sree/bs/argus_demo/brightsign-npu-gaze-extension-ng

# 2. Create test directory structure
mkdir -p tests

# 3. Install Google Test (one-time)
sudo apt-get update
sudo apt-get install -y libgtest-dev cmake

# 4. Create simple test to verify setup
cat > tests/test_simple.cpp << 'EOF'
#include <gtest/gtest.h>

TEST(SimpleTest, BasicAssertion) {
    EXPECT_EQ(1 + 1, 2);
    EXPECT_TRUE(true);
}
EOF

# 5. Create minimal CMakeLists.txt for tests
cat > tests/CMakeLists.txt << 'EOF'
include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(run_all_tests
    test_simple.cpp
)

target_link_libraries(run_all_tests
    gtest
    gtest_main
    pthread
)

add_test(NAME AllTests COMMAND run_all_tests)
EOF

# 6. Update root CMakeLists.txt (add at end)
echo "" >> CMakeLists.txt
echo "# Unit tests" >> CMakeLists.txt
echo "option(BUILD_TESTS \"Build unit tests\" ON)" >> CMakeLists.txt
echo "if(BUILD_TESTS)" >> CMakeLists.txt
echo "    enable_testing()" >> CMakeLists.txt
echo "    add_subdirectory(tests)" >> CMakeLists.txt
echo "endif()" >> CMakeLists.txt

# 7. Build and run
mkdir -p build_tests
cd build_tests
cmake .. -DBUILD_TESTS=ON
make -j4
./tests/run_all_tests
```

#### Daily Testing Workflow

Once setup is complete, your daily workflow becomes:

```bash
# 1. Make code changes to src/*.cpp

# 2. Add/update corresponding tests in tests/*.cpp

# 3. Rebuild and test (from project root)
cd build_tests
make -j4                              # Rebuild
./tests/run_all_tests --gtest_color=yes   # Run all

# OR: Quick one-liner
cd build_tests && make -j4 && ./tests/run_all_tests --gtest_color=yes

# 4. Run specific tests during development
./tests/run_all_tests --gtest_filter=ZoneLogic*        # Zone tests only
./tests/run_all_tests --gtest_filter=*Border*          # All tests with "Border"

# 5. If test fails, debug
gdb ./tests/run_all_tests
(gdb) run --gtest_filter=ZoneLogicTest.BorderHysteresis
(gdb) bt    # Get backtrace
```

#### Test Results Tracking (Manual)

Create a simple test log:

```bash
# Create test log script
cat > scripts/track_test_results.sh << 'EOF'
#!/bin/bash
DATE=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="test_results"
mkdir -p $RESULTS_DIR

cd build_tests

# Run tests and capture output
./tests/run_all_tests --gtest_color=yes 2>&1 | tee $RESULTS_DIR/run_$DATE.log

# Extract summary
echo "=== Test Summary ===" >> $RESULTS_DIR/run_$DATE.log
grep -E "\[  PASSED  \]|\[  FAILED  \]" $RESULTS_DIR/run_$DATE.log

# Generate XML for later analysis
./tests/run_all_tests --gtest_output=xml:$RESULTS_DIR/results_$DATE.xml

echo "Results saved to: $RESULTS_DIR/run_$DATE.log"
EOF

chmod +x scripts/track_test_results.sh

# Use it
./scripts/track_test_results.sh
```

#### Pre-Commit Testing Checklist

Before committing code, run this checklist:

```bash
#!/bin/bash
# pre_commit_tests.sh

echo "=== Pre-Commit Test Suite ==="

# 1. Build
echo "[1/5] Building..."
cd build_tests && make -j4 || { echo "❌ Build failed"; exit 1; }
echo "✅ Build passed"

# 2. Run all unit tests
echo "[2/5] Running unit tests..."
./tests/run_all_tests || { echo "❌ Unit tests failed"; exit 1; }
echo "✅ Unit tests passed"

# 3. Check for memory leaks (if valgrind available)
if command -v valgrind &> /dev/null; then
    echo "[3/5] Checking memory leaks..."
    valgrind --leak-check=full --error-exitcode=1 ./tests/run_all_tests --gtest_filter=ZoneLogic* > /dev/null 2>&1 || {
        echo "⚠️  Memory leaks detected (non-critical)"
    }
else
    echo "[3/5] Skipping memory check (valgrind not installed)"
fi

# 4. Run on sample clip (component test)
echo "[4/5] Testing with sample clip..."
if [ -f "../test_data/clips/clip01_single_pass.mp4" ]; then
    ../build_ls5/attention_demo --input ../test_data/clips/clip01_single_pass.mp4 --config ../configs/config-test.json > /dev/null 2>&1
    echo "✅ Component test passed"
else
    echo "⚠️  Sample clip not found, skipping"
fi

# 5. Summary
echo "[5/5] All checks complete!"
echo ""
echo "✅ Ready to commit"
```

#### Test Progress Tracking

Track your test implementation progress:

```bash
# Create test progress tracker
cat > test_progress.md << 'EOF'
# Test Implementation Progress

## Unit Tests Status

| Component | Test File | Tests Written | Tests Passing | Status |
|-----------|-----------|---------------|---------------|--------|
| Zone Logic | test_zone_logic.cpp | 0/8 | 0/8 | 🔴 Not Started |
| Tracker | test_tracker.cpp | 0/6 | 0/6 | 🔴 Not Started |
| Gaze Logic | test_gaze_logic.cpp | 0/5 | 0/5 | 🔴 Not Started |
| Direction | test_direction.cpp | 0/4 | 0/4 | 🔴 Not Started |
| Dwell Time | test_dwell.cpp | 0/3 | 0/3 | 🔴 Not Started |
| Speed Calc | test_speed.cpp | 0/2 | 0/2 | 🔴 Not Started |

**Legend:**
- 🔴 Not Started
- 🟡 In Progress (< 80% passing)
- 🟢 Complete (≥ 80% passing)
- ✅ Done (100% passing)

## Next Steps
1. [ ] Implement test_zone_logic.cpp
2. [ ] Implement test_tracker.cpp
3. [ ] Add remaining test files
4. [ ] Achieve 80% pass rate
5. [ ] Set up CI/CD automation

Last Updated: YYYY-MM-DD
EOF
```

### Step 10: CI/CD Integration (Future)

**For later when CI/CD is ready:**

**GitHub Actions example (.github/workflows/unit-tests.yml):**

```yaml
name: Unit Tests

on: [push, pull_request]

jobs:
  unit-tests:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v3
      with:
        submodules: recursive
    
    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y cmake libgtest-dev
    
    - name: Build tests
      run: |
        mkdir -p build_tests
        cd build_tests
        cmake .. -DBUILD_TESTS=ON
        make -j4
    
    - name: Run tests
      run: |
        cd build_tests
        ctest --output-on-failure --verbose
    
    - name: Generate report
      if: always()
      run: |
        cd build_tests/tests
        ./run_all_tests --gtest_output=xml:test_results.xml
    
    - name: Upload results
      if: always()
      uses: actions/upload-artifact@v3
      with:
        name: test-results
        path: build_tests/tests/test_results.xml
```

**Note**: Skip CI/CD setup for now. Focus on running tests locally, then automate later.

### Step 10: Mocking External Dependencies (Advanced)

If your code depends on hardware (camera, NPU), use mocks:

**Install Google Mock:**

```bash
# Already included with Google Test
```

**Example mock for camera:**

```cpp
// tests/mocks/mock_camera.h
#include <gmock/gmock.h>
#include "camera_interface.h"

class MockCamera : public CameraInterface {
public:
    MOCK_METHOD(bool, open, (), (override));
    MOCK_METHOD(cv::Mat, capture, (), (override));
    MOCK_METHOD(void, close, (), (override));
};
```

**Use in test:**

```cpp
#include <gmock/gmock.h>
#include "mocks/mock_camera.h"

using ::testing::Return;
using ::testing::_;

TEST(SystemTest, CameraFailure) {
    MockCamera mock_camera;
    
    // Expect open() to be called and return false (failure)
    EXPECT_CALL(mock_camera, open())
        .WillOnce(Return(false));
    
    System sys(&mock_camera);
    bool result = sys.initialize();
    
    EXPECT_FALSE(result);
}
```

### Troubleshooting

**Issue: "gtest/gtest.h not found"**

```bash
# Verify Google Test is installed
dpkg -l | grep gtest

# Or check include path
ls /usr/include/gtest
```

**Issue: "undefined reference to `testing::...`"**

```cmake
# Ensure you're linking Google Test libraries
target_link_libraries(run_all_tests
    GTest::GTest
    GTest::Main  # Important!
    pthread
)
```

**Issue: "Tests compile but crash on run"**

```bash
# Run with debugger
gdb ./tests/run_all_tests
(gdb) run
(gdb) bt  # Get backtrace on crash
```

**Issue: "Tests pass locally but fail in CI"**

- Check architecture differences (x86 vs ARM)
- Verify all dependencies installed
- Check for race conditions (use `--gtest_repeat`)
- Verify filesystem paths

### Quick Start Script

Create `run_tests.sh` in project root:

```bash
#!/bin/bash
# run_tests.sh - Quick test runner

set -e

echo "=== Building Unit Tests ==="
mkdir -p build_tests
cd build_tests
cmake .. -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

echo ""
echo "=== Running Unit Tests ==="
./tests/run_all_tests --gtest_color=yes "$@"

echo ""
echo "=== Test Summary ==="
./tests/run_all_tests --gtest_list_tests | grep -E "^[A-Z]" | wc -l
echo "test suites found."
```

**Usage:**

```bash
# Make executable
chmod +x run_tests.sh

# Run all tests
./run_tests.sh

# Run specific filter
./run_tests.sh --gtest_filter=ZoneLogic*

# Run with repeat
./run_tests.sh --gtest_repeat=10
```

---

## 3. Component Tests (Model + Tracker on Recorded Clips)

**Goal**: Validate model inference, tracking, and event generation on representative video clips  
**Runtime**: 2-3 hours  
**Framework**: Python test harness + MQTT capture

### 3.1 Test Clips (Ground Truth Required)

| Clip | Duration | Scenario | GT Labels |
|------|----------|----------|-----------|
| `clip01_single_pass.mp4` | 15s | One person walks in, pauses, walks out | Enter @ 2.3s, Exit @ 12.1s |
| `clip02_hover_border.mp4` | 20s | Person hovers at ROI boundary | 0 enters, 0 exits |
| `clip03_multi_cross.mp4` | 30s | Two people cross in opposite directions | 2 enters, 2 exits |
| `clip04_occlusion.mp4` | 25s | Person partially occluded by pillar | 1 enter, 1 exit, ID stable |
| `clip05_backlight.mp4` | 20s | Backlit scenario (bright window) | Detection recall test |
| `clip06_lowlight.mp4` | 20s | Low light (evening/night) | Detection recall test |
| `clip07_mask_hat.mp4` | 15s | Person wearing mask and hat | Detection + gaze test |
| `clip08_side_profile.mp4` | 15s | Person enters sideways | Direction test |
| `clip09_group_burst.mp4` | 30s | 3-5 people enter together | Multi-track test |
| `clip10_gaze_on.mp4` | 20s | Person looks at screen continuously | Gaze fixation test |
| `clip11_gaze_off.mp4` | 20s | Person faces away from screen | Gaze negative test |
| `clip12_long_dwell.mp4` | 60s | Person stays in ROI for 1 minute | Dwell accuracy test |

**Ground Truth Format (CSV):**

```csv
clip,timestamp_ms,event_type,track_id,bbox_x,bbox_y,bbox_w,bbox_h,gaze,notes
clip01_single_pass.mp4,2300,enter,1,320,240,80,200,false,Person enters from left
clip01_single_pass.mp4,12100,exit,1,560,240,80,200,false,Person exits to right
```

### 3.2 Metrics

#### 3.2.1 People Detection (YOLOX)

**mAP@0.5**: Mean Average Precision at IoU threshold 0.5

```python
def calculate_map(predictions, ground_truth):
    """
    predictions: List of {frame, bbox, confidence, class}
    ground_truth: List of {frame, bbox, class}
    """
    for threshold in [0.5]:
        matches = match_detections(predictions, ground_truth, iou_threshold=threshold)
        precision = len(matches) / len(predictions)
        recall = len(matches) / len(ground_truth)
        ap = calculate_average_precision(precision, recall)
    return ap
```

**Target**: mAP@0.5 ≥ 0.85

#### 3.2.2 Tracking (ID Persistence)

```python
from motmetrics import MOTMetrics

def evaluate_tracking(predictions, ground_truth):
    """
    predictions: List of {frame, track_id, bbox}
    ground_truth: List of {frame, person_id, bbox}
    """
    acc = motmetrics.MOTAccumulator(auto_id=True)
    
    for frame in range(max_frame):
        pred_ids = [p.track_id for p in predictions if p.frame == frame]
        pred_boxes = [p.bbox for p in predictions if p.frame == frame]
        gt_ids = [g.person_id for g in ground_truth if g.frame == frame]
        gt_boxes = [g.bbox for g in ground_truth if g.frame == frame]
        
        distances = calculate_iou_distances(gt_boxes, pred_boxes)
        acc.update(gt_ids, pred_ids, distances)
    
    metrics = motmetrics.compute(acc, metrics=['idf1', 'mota', 'num_switches'])
    return metrics
```

**Targets:**

- **IDF1** ≥ 0.85 (ID F1 Score)
- **ID switches** ≤ 12 per hour

#### 3.2.3 Event Accuracy (Enter/Exit)

```python
def evaluate_events(pred_events, gt_events, tolerance_ms=500):
    """
    Match predicted events to ground truth within ±500ms tolerance
    """
    true_positives = 0
    false_positives = 0
    false_negatives = 0
    
    for gt in gt_events:
        matches = [p for p in pred_events 
                   if p.event_type == gt.event_type
                   and abs(p.timestamp_ms - gt.timestamp_ms) <= tolerance_ms]
        
        if matches:
            true_positives += 1
            pred_events.remove(matches[0])
        else:
            false_negatives += 1
    
    false_positives = len(pred_events)
    
    precision = true_positives / (true_positives + false_positives)
    recall = true_positives / (true_positives + false_negatives)
    f1 = 2 * (precision * recall) / (precision + recall)
    
    return {'precision': precision, 'recall': recall, 'f1': f1}
```

**Targets:**

- Enter F1 ≥ 0.97
- Exit F1 ≥ 0.97

#### 3.2.4 Gaze Estimation

```python
def evaluate_gaze(pred_gaze, gt_gaze):
    """Frame-level precision/recall"""
    tp = sum(1 for p, g in zip(pred_gaze, gt_gaze) 
             if p.gaze_state and g.looking_at_screen)
    fp = sum(1 for p, g in zip(pred_gaze, gt_gaze) 
             if p.gaze_state and not g.looking_at_screen)
    fn = sum(1 for p, g in zip(pred_gaze, gt_gaze) 
             if not p.gaze_state and g.looking_at_screen)
    
    precision = tp / (tp + fp) if (tp + fp) > 0 else 0
    recall = tp / (tp + fn) if (tp + fn) > 0 else 0
    
    return {'precision': precision, 'recall': recall}
```

**Targets:**

- Precision ≥ 0.90
- Recall ≥ 0.85
- Fixation latency (mean) ≤ 20 frames (~667ms @ 30fps)

#### 3.2.5 Performance

```python
def measure_latency(mqtt_messages, video_timestamps):
    """E2E latency from capture to MQTT publish"""
    latencies = []
    
    for msg in mqtt_messages:
        if msg.frame_id in video_timestamps:
            latency = msg.mqtt_timestamp_ms - video_timestamps[msg.frame_id]
            latencies.append(latency)
    
    return {
        'p50': np.percentile(latencies, 50),
        'p95': np.percentile(latencies, 95),
        'p99': np.percentile(latencies, 99),
        'mean': np.mean(latencies)
    }
```

**Targets:**

- FPS: mean ≥ 20, min ≥ 15
- Latency: P50 ≤ 120ms, P95 ≤ 180ms

### 3.3 Test Execution

```bash
# Run component tests on all clips
./test_harness.py \
    --clips "test_data/clips/*.mp4" \
    --ground-truth "test_data/gt_labels.csv" \
    --config "configs/config-test.json" \
    --output "results/component_tests.json"
```

```python
#!/usr/bin/env python3
def run_component_test(clip_path, gt_path, config_path):
    # Start MQTT collector
    collector = MQTTCollector(topic="bs/argus/analytics")
    collector.start()
    
    # Run attention_demo with clip as input
    cmd = [
        "./attention_demo",
        "--input", clip_path,
        "--config", config_path,
        "--mqtt-broker", "localhost:1883"
    ]
    subprocess.run(cmd, timeout=120)
    
    # Stop collector and get messages
    collector.stop()
    mqtt_events = collector.get_messages()
    
    # Load ground truth
    gt_events = load_ground_truth(gt_path)
    
    # Evaluate
    results = {
        'detection': evaluate_detection(mqtt_events, gt_events),
        'tracking': evaluate_tracking(mqtt_events, gt_events),
        'events': evaluate_events(mqtt_events, gt_events),
        'gaze': evaluate_gaze(mqtt_events, gt_events),
        'performance': evaluate_performance(mqtt_events)
    }
    
    return results
```

---

## 4. System / E2E Tests (Live Camera)

**Goal**: Validate complete system with real hardware and users  
**Runtime**: 4-6 hours  
**Location**: Test lab with marked zones and scripted scenarios

### 4.1 Test Setup

**Hardware:**

- BrightSign HS145 (RK3568) or XT5 (RK3588)
- USB camera (1920x1080 @ 30fps) or RTSP stream
- Test display with marked ROI boundary (chalk/tape on floor)
- MQTT broker for event collection
- Screen recording for overlay validation

**Configuration:**

```json
{
    "roi": {
        "x": 192,
        "y": 144,
        "width": 448,
        "height": 336
    },
    "zone_config": {
        "hysteresis_margin_px": 8,
        "debounce_frames": 5,
        "entry_threshold_frames": 5,
        "exit_threshold_frames": 5
    },
    "gaze_config": {
        "fixation_frames": 15,
        "confidence_threshold": 0.7
    },
    "tracker_config": {
        "iou_threshold": 0.5,
        "max_age_frames": 30,
        "min_hits": 3
    }
}
```

### 4.2 Scripted Test Scenarios

#### Scenario 1: Straight Pass-In / Pass-Out (Single Subject)

**Procedure:**

1. Subject starts 2 meters outside ROI boundary
2. Walk straight into ROI at normal pace (~1.2 m/s)
3. Stop in center of ROI for 5 seconds
4. Walk straight out of ROI

**Expected:**

- ✅ One `enter` event when crossing boundary inward
- ✅ No duplicate `enter` events while inside
- ✅ One `exit` event when crossing boundary outward
- ✅ Dwell time ≈ 5 seconds (±1s tolerance)
- ✅ Direction: matches entry direction (e.g., LEFT or RIGHT)

**Acceptance**: 10/10 passes with correct events

#### Scenario 2: Hover at Border (Hysteresis Test)

**Procedure:**

1. Subject stands just outside ROI boundary
2. Lean forward 10cm (partially crosses boundary)
3. Lean back 10cm (returns outside)
4. Repeat 5 times over 30 seconds

**Expected:**

- ✅ Zero `enter` events (hysteresis prevents false triggers)
- ✅ Zero `exit` events
- ✅ Track persists throughout (no ID switches)

**Acceptance**: 0 false events in 5/5 repetitions

#### Scenario 3: Two People Cross Opposite Directions (Collision Test)

**Procedure:**

1. Subject A enters from left
2. Subject B enters from right simultaneously
3. Subjects pass each other in center of ROI
4. Subject A exits right, Subject B exits left

**Expected:**

- ✅ Two `enter` events (one per subject)
- ✅ Two distinct track IDs maintained throughout
- ✅ Two `exit` events (one per subject)
- ✅ Zero ID switches during collision
- ✅ Directions: Subject A = RIGHT, Subject B = LEFT

**Acceptance**: 8/10 passes with correct tracking

#### Scenario 4: Occlusion by Pillar (Robustness Test)

**Procedure:**

1. Subject enters ROI
2. Walk behind pillar/poster (50% body occluded for 2 seconds)
3. Emerge from behind occlusion
4. Exit ROI

**Expected:**

- ✅ Track ID maintained before and after occlusion
- ✅ One `enter` event (no duplicate after occlusion)
- ✅ One `exit` event
- ✅ Dwell time continuous (not reset by occlusion)

**Acceptance**: Track ID stable in 7/10 passes

#### Scenario 5: Face Away vs Toward Display (Gaze Test)

**Procedure:**

1. Subject enters ROI facing away from display
2. Stand for 5 seconds (no gaze expected)
3. Turn to face display
4. Look at display for 10 seconds
5. Turn away and exit

**Expected:**

- ✅ Gaze = false for first 5 seconds
- ✅ Gaze = true after fixation period (~500ms)
- ✅ Gaze remains true for ~9.5 seconds
- ✅ Gaze = false when turning away

**Acceptance**: Gaze transitions correct in 8/10 passes

#### Scenario 6: Group Entry (3-5 People Burst)

**Procedure:**

1. Group of 3-5 people enters ROI together
2. All walk in same direction
3. Some walk faster, creating slight stagger
4. All exit together

**Expected:**

- ✅ N `enter` events (one per person, N = group size)
- ✅ N distinct track IDs
- ✅ N `exit` events
- ✅ ID switches ≤ 1 per group (tolerable with occlusion)

**Acceptance**: Detection count ≥ 80% of actual count

### 4.3 Acceptance Criteria

| Scenario | Metric | Target | Pass Threshold |
|----------|--------|--------|----------------|
| 1. Straight pass | Enter/Exit F1 | 1.0 | ≥ 0.95 |
| 2. Hover border | False events | 0 | ≤ 1 per 30s |
| 3. Multi-cross | ID switches | 0 | ≤ 1 per pair |
| 4. Occlusion | ID stability | 100% | ≥ 70% |
| 5. Gaze | Gaze accuracy | 100% | ≥ 80% |
| 6. Group | Detection count | 100% | ≥ 80% |
| **Overall** | Enter/Exit F1 | ≥ 0.98 | ≥ 0.95 |
| **Overall** | FPS | ≥ 20 | ≥ 15 |
| **Overall** | E2E Latency P95 | ≤ 180ms | ≤ 250ms |

---

## 4.4 Manual Test Scenarios (Exploratory Testing)

**Goal**: Discover edge cases and usability issues not covered by scripted tests  
**Duration**: 8-12 hours total  
**Tester**: QA Engineer with domain knowledge  
**Approach**: Exploratory + checklist-based

### Test Categories

#### Category A: Environmental Variations (2-3 hours)

**Objective**: Verify robustness across lighting and environmental conditions

**Test Scenarios:**

| # | Scenario | Steps | Expected Behavior | Priority |
|---|----------|-------|-------------------|----------|
| A1 | **Sudden lighting change** | 1. Start with normal lighting<br>2. Turn off overhead lights<br>3. Observe for 30s<br>4. Turn lights back on | - Detection continues<br>- No false exits<br>- ID maintained | HIGH |
| A2 | **Backlight from window** | 1. Position camera toward bright window<br>2. Person walks between camera and window<br>3. Track entry/exit | - Person detected (silhouette)<br>- Events fire correctly<br>- May have lower confidence | MEDIUM |
| A3 | **Reflections on floor** | 1. Wet or polished floor creating reflections<br>2. Walk through ROI | - No false person detection<br>- Single track ID | MEDIUM |
| A4 | **Moving shadows** | 1. Person walks past with strong directional light<br>2. Shadow cast on ROI | - Shadow not detected as person<br>- Events triggered by person only | HIGH |
| A5 | **Flickering fluorescent** | 1. Simulate flickering light<br>2. Person enters ROI | - FPS stable<br>- No dropped frames<br>- Events fire normally | LOW |

#### Category B: Human Behavior Variations (3-4 hours)

**Objective**: Test realistic but unpredictable human movements

| # | Scenario | Steps | Expected Behavior | Priority |
|---|----------|-------|-------------------|----------|
| B1 | **Walking backward** | 1. Person enters ROI walking backward<br>2. Face away from camera | - Detection successful<br>- Direction = BACKWARD<br>- Gaze = false | MEDIUM |
| B2 | **Crawling/crouching** | 1. Person crawls or crouches into ROI<br>2. Bbox height significantly reduced | - Detection may fail (acceptable)<br>- OR: Detection with low confidence | LOW |
| B3 | **Jumping/waving arms** | 1. Person jumps or waves arms in ROI<br>2. Bbox changes rapidly | - ID maintained<br>- No false exits<br>- Bbox adapts | MEDIUM |
| B4 | **Carrying large object** | 1. Person carries box/poster/bag<br>2. Object partially occludes body | - Person detected<br>- Not counted as 2 people<br>- ID stable | HIGH |
| B5 | **Pushing stroller/wheelchair** | 1. Person pushes stroller or wheelchair<br>2. Two bboxes expected | - 1-2 detections acceptable<br>- Count documented<br>- IDs stable | MEDIUM |
| B6 | **Sitting down in ROI** | 1. Person enters<br>2. Sits on floor/chair in ROI<br>3. Stays 60s<br>4. Stands and exits | - Enter event fires<br>- Dwell continues while seated<br>- Detection may flicker (bbox smaller)<br>- Exit fires | HIGH |
| B7 | **Running through quickly** | 1. Sprint through ROI (< 1s transit) | - Enter AND exit both fire<br>- Short dwell time (< 1s) | HIGH |
| B8 | **Spinning in place** | 1. Enter ROI<br>2. Spin 360° multiple times<br>3. Exit | - ID stable despite rotation<br>- Direction may be UNKNOWN<br>- Gaze fluctuates | LOW |
| B9 | **Stopping at boundary** | 1. Walk toward ROI<br>2. Stop with feet just outside<br>3. Lean torso inside<br>4. Retreat | - Zero enter events (footpoint rule)<br>- OR: One enter if torso triggers | MEDIUM |
| B10 | **Two people holding hands** | 1. Couple walks hand-in-hand<br>2. Close proximity (< 20cm gap) | - Two distinct tracks<br>- OR: Merged into one (document) | MEDIUM |

#### Category C: Clothing & Appearance (1-2 hours)

**Objective**: Test detection across diverse appearances

| # | Scenario | Steps | Expected Behavior | Priority |
|---|----------|-------|-------------------|----------|
| C1 | **All black clothing** | Person wears all black in low light | - Detection may be challenging<br>- Document recall rate | MEDIUM |
| C2 | **Highly reflective clothing** | Person wears reflective vest/jacket | - Detection successful<br>- No false bbox artifacts | LOW |
| C3 | **Patterned clothing** | Person wears striped/checkered patterns | - Detection normal<br>- No bbox jitter | LOW |
| C4 | **Costume/mascot** | Person in large costume (non-human shape) | - May not detect (acceptable)<br>- OR: Low confidence detection | LOW |
| C5 | **Facial coverings** | Person wears mask, sunglasses, hat | - Person detection normal<br>- Face detection may fail<br>- Gaze = false (expected) | HIGH |
| C6 | **Height extremes** | Test with children (< 1m) and tall adults (> 2m) | - Both detected<br>- Bbox adapts to height | MEDIUM |

#### Category D: Edge Cases & Stress Tests (2-3 hours)

**Objective**: Find breaking points and unexpected behaviors

| # | Scenario | Steps | Expected Behavior | Priority |
|---|----------|-------|-------------------|----------|
| D1 | **ROI fully occupied** | 1. 5-10 people stand in ROI simultaneously<br>2. All exit together | - All detected OR partial (document limit)<br>- FPS may drop<br>- All exit events fire | HIGH |
| D2 | **Rapid enter/exit cycles** | 1. Person enters<br>2. Exits immediately<br>3. Re-enters within 2s<br>4. Repeat 10 times | - All enters/exits fire<br>- ID may change (acceptable)<br>- No crashes | HIGH |
| D3 | **Long-term stationary** | 1. Person enters<br>2. Stands still for 10 minutes<br>3. Exits | - Enter fires<br>- Dwell = ~10 min<br>- ID stable<br>- Exit fires | MEDIUM |
| D4 | **Camera nudged** | 1. System running<br>2. Physically nudge camera (5-10°)<br>3. Observe recovery | - Brief confusion (acceptable)<br>- Tracking recovers within 5s<br>- No false events | HIGH |
| D5 | **Partial FOV blockage** | 1. Place object blocking 25% of FOV<br>2. Person walks through visible area | - Detection in visible area<br>- Events fire if ROI still visible | MEDIUM |
| D6 | **Zero people for extended time** | 1. No activity for 30 minutes<br>2. Check system health | - FPS stable<br>- Memory not growing<br>- MQTT heartbeat (if configured) | MEDIUM |
| D7 | **Continuous traffic for 1 hour** | 1. 20-30 people/hour steady traffic<br>2. Monitor metrics | - FPS stable<br>- Memory stable<br>- Event count matches manual count ±5% | HIGH |
| D8 | **Pets/animals** | Dog or cat walks through ROI | - Not detected as person (class filter)<br>- OR: Detected but very low confidence | LOW |
| D9 | **Moving objects (non-person)** | Cart, robot, rolling ball through ROI | - Not detected as person<br>- No false events | MEDIUM |
| D10 | **Camera covered briefly** | 1. Cover lens for 5s<br>2. Uncover<br>3. Observe recovery | - No crashes<br>- Resumes tracking within 3s<br>- No false events | HIGH |

#### Category E: Configuration & Operations (1-2 hours)

**Objective**: Test runtime configuration changes and system operations

| # | Scenario | Steps | Expected Behavior | Priority |
|---|----------|-------|-------------------|----------|
| E1 | **Config change (ROI resize)** | 1. System running<br>2. Edit config.json (change ROI)<br>3. Wait for auto-reload | - Config detected within 5s<br>- Extension restarts (exit code 42)<br>- New ROI applied<br>- Total downtime < 15s | HIGH |
| E2 | **Config change (invalid JSON)** | 1. Edit config.json with syntax error<br>2. Wait for detection | - Extension logs error<br>- Falls back to previous config OR defaults<br>- No crash | HIGH |
| E3 | **MQTT broker unavailable** | 1. Stop MQTT broker<br>2. Generate events<br>3. Restart broker | - Extension logs connection errors<br>- Events buffered (if enabled)<br>- Reconnects automatically<br>- Buffered events published | HIGH |
| E4 | **Disk full** | 1. Fill /storage/sd partition<br>2. Trigger events | - Extension handles write failures<br>- Logs to console/syslog<br>- No crash | MEDIUM |
| E5 | **Network disconnect** | 1. Disconnect Ethernet<br>2. Generate events<br>3. Reconnect | - Local processing continues<br>- MQTT buffered<br>- Reconnects automatically | MEDIUM |
| E6 | **Timezone change** | 1. Change system timezone<br>2. Generate events | - Timestamps in MQTT correct<br>- OR: UTC timestamps (preferred) | LOW |
| E7 | **System reboot** | 1. Generate traffic<br>2. Reboot device<br>3. Verify auto-start | - Extension auto-starts<br>- Config loaded<br>- Tracking begins within 30s | HIGH |

#### Category F: Gaze-Specific Scenarios (1-2 hours)

**Objective**: Validate gaze detection edge cases

| # | Scenario | Steps | Expected Behavior | Priority |
|---|----------|-------|-------------------|----------|
| F1 | **Side profile (90° angle)** | 1. Person enters sideways<br>2. Face perpendicular to camera<br>3. Walk through ROI | - Face detection may fail (acceptable)<br>- Gaze = false<br>- Person detection successful | MEDIUM |
| F2 | **Looking down at phone** | 1. Enter ROI looking at phone<br>2. Face angled 45° down<br>3. Stay 10s | - Face detected (maybe)<br>- Gaze = false (correct) | HIGH |
| F3 | **Glancing vs fixation** | 1. Enter ROI<br>2. Glance at screen (< 3 frames)<br>3. Look away<br>4. Fixate on screen (> 15 frames) | - Brief glance: gaze = false<br>- Fixation: gaze = true after delay | HIGH |
| F4 | **Multiple displays** | 1. Multiple screens in scene<br>2. Person looks at different screen<br>3. ROI-specific gaze detection | - Gaze associated with person in ROI<br>- Other displays ignored | MEDIUM |
| F5 | **Sunglasses** | Person wears dark sunglasses | - Face detection may succeed<br>- Gaze confidence low<br>- Likely gaze = false | MEDIUM |
| F6 | **Head tilt** | Person tilts head 30-45° while looking | - Face detection robust<br>- Gaze may be inaccurate (document) | LOW |
| F7 | **Far distance (> 5m)** | Person stands far from camera | - Face bbox very small<br>- Gaze may fail (acceptable) | LOW |

### Manual Testing Checklist

**Pre-Test Setup:**

- [ ] BrightSign device powered and network-connected
- [ ] Camera connected and streaming (verify with `v4l2-ctl --list-devices`)
- [ ] MQTT broker running and accessible
- [ ] Config file validated (ROI, thresholds)
- [ ] Logs accessible (`tail -f /storage/sd/logs/gaze.log`)
- [ ] Test area marked (ROI boundary on floor)
- [ ] Recording device ready (for documentation)
- [ ] Stopwatch/timer available
- [ ] Test subjects briefed

**During Testing:**

- [ ] Log all test IDs and timestamps
- [ ] Record unexpected behaviors
- [ ] Capture screenshots/videos of issues
- [ ] Note FPS/latency periodically
- [ ] Monitor memory usage: `free -m`
- [ ] Check CPU temp: `cat /sys/class/thermal/thermal_zone0/temp`

**Post-Test:**

- [ ] Export MQTT logs: `mosquitto_sub -h localhost -t 'bs/argus/analytics' -C 1000 > mqtt_dump.jsonl`
- [ ] Collect system logs: `cp /storage/sd/logs/gaze.log ./test_results/`
- [ ] Analyze false positives/negatives
- [ ] Document pass/fail for each scenario
- [ ] Calculate aggregate metrics
- [ ] File bugs for critical issues
- [ ] Update test cases based on findings

### Manual Test Report Template

```markdown
## Manual Test Report

**Date**: YYYY-MM-DD  
**Tester**: [Name]  
**Device**: BrightSign HS145 / XT5  
**Firmware**: vX.Y.Z  
**Extension Version**: argus-dev-XXXXXXXXXX  
**Duration**: X hours

- Location: [Lab/Field]
- Camera: [Model, Resolution, FPS]
- Lighting: [Normal/Low/Backlit]
- ROI: [x, y, w, h]

- **Total Scenarios**: X
- **Passed**: X (Y%)
- **Failed**: X (Y%)
- **Blocked**: X

1. [Issue title] - Priority: HIGH/MEDIUM/LOW
   - Scenario: [Test ID]
   - Expected: [Description]
   - Actual: [Description]
   - Frequency: X/10 attempts

| Category | Pass | Fail | Pass Rate |
|----------|------|------|-----------|
| Environmental | X | X | X% |
| Behavior | X | X | X% |
| Clothing | X | X | X% |
| Edge Cases | X | X | X% |
| Configuration | X | X | X% |
| Gaze | X | X | X% |

- FPS: min X, max Y, avg Z
- Latency: P95 = X ms
- Memory growth: X MB/hour
- False events: X per hour
- ID switches: X per hour

1. [Recommendation 1]
2. [Recommendation 2]

- MQTT logs: `mqtt_dump_YYYYMMDD.jsonl`
- System logs: `gaze_log_YYYYMMDD.txt`
- Screenshots: `screenshots/`
- Videos: `videos/`
```

### Tips for Effective Manual Testing

**Test Execution:**

1. **Start simple, progress to complex** - Validate basic scenarios before edge cases
2. **Use consistent test subjects** - Same person for reproducibility
3. **Vary test subjects** - Different heights, clothing, ages
4. **Record everything** - Video entire test session for review
5. **Take breaks** - Fatigue reduces test quality
6. **Retest failures** - Confirm issues are reproducible (3x minimum)

**Issue Documentation:**

1. **Be specific** - "Enter event fires 2s late" vs "Events are wrong"
2. **Include context** - FPS, lighting, number of people
3. **Severity classification**:
   - **CRITICAL**: Crashes, data corruption, safety issues
   - **HIGH**: Wrong events, major accuracy issues
   - **MEDIUM**: Intermittent issues, degraded performance
   - **LOW**: Minor UX issues, edge cases

**Metrics to Monitor:**

```bash
# FPS (from logs)
grep "FPS:" /storage/sd/logs/gaze.log | tail -n 100 | awk '{sum+=$3; count++} END {print "Avg FPS:", sum/count}'

# Memory
watch -n 10 'free -m | grep Mem'

# MQTT rate
mosquitto_sub -h localhost -t 'bs/argus/analytics' | while read msg; do date +%s; done | uniq -c

# Error count
grep ERROR /storage/sd/logs/gaze.log | wc -l
```

**Common Pitfalls:**

- ❌ Testing with single person only (misses multi-track issues)
- ❌ Perfect lighting only (misses low-light failures)
- ❌ Short test duration (misses memory leaks)
- ❌ Not documenting exact steps (can't reproduce)
- ❌ Ignoring "minor" issues (often indicators of bigger problems)

---

## 5. Soak / Reliability Tests

**Goal**: Validate long-term stability, memory leaks, and error handling  
**Runtime**: 8-12 hours  
**Setup**: Live camera or looped RTSP playlist

### 5.1 Monitored Metrics

#### Memory Growth

```bash
# Monitor RSS memory every minute
while true; do
    ps aux | grep attention_demo | awk '{print $6}' >> /tmp/memory_log.txt
    sleep 60
done

# Calculate growth rate
python3 << EOF
import numpy as np
data = np.loadtxt('/tmp/memory_log.txt')
hours = len(data) / 60.0
growth_mb = (data[-1] - data[0]) / 1024.0
rate = growth_mb / hours
print(f"Memory growth: {rate:.2f} MB/hour")
EOF
```

**Target**: < 20 MB/hour growth

#### FPS Stability

```python
def monitor_fps_stability(mqtt_messages, duration_hours):
    """Track FPS over time, detect drops"""
    fps_windows = []
    
    for hour in range(int(duration_hours)):
        hour_messages = [m for m in mqtt_messages 
                         if hour * 3600 <= m.timestamp < (hour + 1) * 3600]
        fps = len(hour_messages) / 3600.0
        fps_windows.append(fps)
    
    return {
        'mean_fps': np.mean(fps_windows),
        'min_fps': np.min(fps_windows),
        'std_fps': np.std(fps_windows),
        'drops_below_18': sum(1 for f in fps_windows if f < 18)
    }
```

**Target**: FPS never drops below 18 for > 5 minutes

#### MQTT Publish Gaps

```python
def detect_mqtt_gaps(mqtt_messages, threshold_seconds=3):
    """Find gaps in MQTT publishes that exceed threshold"""
    gaps = []
    
    for i in range(1, len(mqtt_messages)):
        gap = mqtt_messages[i].timestamp - mqtt_messages[i-1].timestamp
        if gap > threshold_seconds:
            gaps.append(gap)
    
    return {
        'num_gaps': len(gaps),
        'max_gap_seconds': max(gaps) if gaps else 0,
        'gaps': gaps
    }
```

**Target**: No gaps > 3 seconds (unless no people in scene)

#### Thermal & CPU

```bash
# Monitor SoC temperature
watch -n 60 cat /sys/class/thermal/thermal_zone0/temp

# Monitor CPU usage
top -b -d 60 | grep attention_demo >> /tmp/cpu_log.txt
```

**Target**: Temp < 75°C, CPU < 80% sustained

### 5.2 Error Injection Tests

#### Camera Disconnect/Reconnect

```bash
# Simulate camera disconnect
sudo modprobe -r uvcvideo
sleep 10
sudo modprobe uvcvideo
```

**Expected:**

- ✅ Extension detects disconnect
- ✅ Enters reconnect loop (logs warning)
- ✅ Recovers automatically when camera returns
- ✅ No crash, no memory leak

#### MQTT Broker Disconnect

```bash
# Stop broker
systemctl stop mosquitto
sleep 60
systemctl start mosquitto
```

**Expected:**

- ✅ Extension buffers events locally (if configured)
- ✅ Reconnects to broker automatically
- ✅ Publishes buffered events after reconnect
- ✅ No data loss for critical events

#### Config File Change (Auto-Reload)

```bash
# Edit config.json
vi /storage/sd/configs/config.json
# Change ROI coordinates

# Wait 5 seconds (config monitor poll interval)
```

**Expected** (if auto-reload enabled):

- ✅ Config change detected within 5 seconds
- ✅ Extension exits with code 42
- ✅ Wrapper script restarts extension
- ✅ New config loaded
- ✅ Total restart time < 15 seconds

#### Disk Full

```bash
# Fill /storage/sd (log partition)
dd if=/dev/zero of=/storage/sd/fillfile bs=1M count=100
```

**Expected:**

- ✅ Extension handles write errors gracefully
- ✅ Falls back to syslog or console
- ✅ No crash

### 5.3 Acceptance Criteria

| Metric | Target | Critical Threshold |
|--------|--------|-------------------|
| Uptime | 100% | ≥ 99.5% |
| Crashes | 0 | ≤ 1 |
| Memory growth | < 20 MB/hour | < 50 MB/hour |
| FPS drops (> 5 min) | 0 | ≤ 2 |
| MQTT gaps (> 3s) | ≤ 6/hour | ≤ 12/hour |
| SoC temperature | < 75°C | < 85°C |
| Auto-recovery | 100% | ≥ 95% |

---

## 6. Edge Cases & Corner Scenarios

### 6.1 Border Ambiguity

**Test**: Person straddles border for ≥30 frames (1 second)

**Expected**: Exactly ONE enter or exit when final position commits (no oscillation)

### 6.2 Partial Body / Truncated Bboxes

**Test**: Bbox partially outside frame (e.g., person at edge of FOV)

**Expected**: Consistent behavior using footpoint (x + w/2, y + h)

### 6.3 Re-entry After Brief Exit

**Test**: Exit → re-enter within N frames (e.g., 30 frames = 1 second)

**Expected**: New `enter` event fires, ID maintained (if tracking is good)

### 6.4 Camera Jolt / FOV Shift

**Test**: Sudden camera movement causes all tracks to shift

**Expected**: Hysteresis prevents false events during camera shake

### 6.5 Multiple Faces Per Person

**Test**: Person with reflection or poster showing multiple faces

**Expected**: Associate highest-confidence face, ignore duplicates

---

## 7. Ground Truth Requirements

### 7.1 Labeling Format

#### Detection Labels (bbox per frame):

```json
{
    "frame": 123,
    "person_id": 1,
    "bbox": [320, 240, 80, 200],
    "occluded": false,
    "truncated": false
}
```

#### Event Labels (timestamp + type):

```csv
clip,timestamp_ms,event_type,person_id,notes
clip01.mp4,2300,enter,1,"Feet cross chalk line"
clip01.mp4,12100,exit,1,"Feet cross chalk line outward"
```

**Important**: Mark timestamp when **feet** cross the ROI boundary

#### Gaze Labels (binary per frame):

```csv
clip,frame,person_id,looking_at_screen
clip10_gaze_on.mp4,100,1,true
clip10_gaze_on.mp4,101,1,true
clip11_gaze_off.mp4,100,2,false
```

__Rule__: `looking_at_screen=true` if face orientation within ±45° of display normal

#### Tracking Labels (ID persistence):

```json
{
    "frame": 123,
    "person_id": 1,  // Consistent across frames
    "bbox": [320, 240, 80, 200]
}
```

### 7.2 Ground Truth Assets

**Minimum Required:**

- **12-20 short clips** (10-30s each):

   - 4 clips: Day lighting, normal traffic
   - 2 clips: Night/low light
   - 2 clips: Backlit scenarios
   - 2 clips: Occlusions (pillar, poster, crowd)
   - 2 clips: Edge cases (hover, quick reentry, group burst)
   - 2 clips: Gaze focus (on/off screen)

- **1 long loop** (10-15 minutes):

   - Continuous moderate traffic for soak testing
   - Representative of real deployment

**Storage Structure:**

```ini
test_data/
├── clips/
│   ├── clip01_single_pass.mp4
│   ├── clip02_hover_border.mp4
│   └── ...
├── ground_truth/
│   ├── detections/
│   │   └── clip01_detections.json
│   ├── events/
│   │   └── events.csv
│   ├── gaze/
│   │   └── gaze_labels.csv
│   └── tracking/
│       └── mot_labels.csv
└── README.md
```

---

## 8. Test Tooling

### 8.1 Replay Harness

**Purpose**: Feed MP4 clips to pipeline with real-time pacing

```python
#!/usr/bin/env python3
import cv2
import time
import subprocess

def replay_with_timing(input_video, config, mqtt_broker):
    """
    Read video, pass frames to attention_demo
    Maintain original frame timing
    """
    cap = cv2.VideoCapture(input_video)
    fps = cap.get(cv2.CAP_PROP_FPS)
    frame_duration_ms = 1000.0 / fps
    
    # Start attention_demo
    proc = subprocess.Popen([
        './attention_demo',
        '--input', input_video,  # Or FIFO pipe
        '--config', config,
        '--mqtt-broker', mqtt_broker
    ])
    
    frame_count = 0
    start_time = time.time()
    
    while cap.isOpened():
        ret, frame = cap.read()
        if not ret:
            break
        
        frame_count += 1
        
        # Maintain original timing
        expected_time = start_time + (frame_count * frame_duration_ms / 1000.0)
        actual_time = time.time()
        if actual_time < expected_time:
            time.sleep(expected_time - actual_time)
    
    cap.release()
    proc.wait()
```

### 8.2 MQTT Capture & Matcher

```python
#!/usr/bin/env python3
import paho.mqtt.client as mqtt
import json

class MQTTCollector:
    def __init__(self, broker='localhost', port=1883, topic='bs/argus/analytics'):
        self.client = mqtt.Client()
        self.topic = topic
        self.messages = []
        
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.connect(broker, port)
    
    def _on_connect(self, client, userdata, flags, rc):
        print(f"Connected to MQTT: {self.topic}")
        client.subscribe(self.topic)
    
    def _on_message(self, client, userdata, msg):
        payload = json.loads(msg.payload.decode())
        self.messages.append(payload)
    
    def start(self):
        self.client.loop_start()
    
    def stop(self):
        self.client.loop_stop()
    
    def get_messages(self):
        return self.messages

def match_events(pred_events, gt_events, tolerance_ms=500):
    """Match predicted MQTT events to ground truth"""
    matches = []
    unmatched_pred = list(pred_events)
    unmatched_gt = list(gt_events)
    
    for gt in gt_events:
        best_match = None
        best_distance = tolerance_ms + 1
        
        for pred in unmatched_pred:
            if pred['type'] != gt['type']:
                continue
            
            time_dist = abs(pred['timestamp'] - gt['timestamp'])
            if time_dist < best_distance:
                best_distance = time_dist
                best_match = pred
        
        if best_match:
            matches.append((gt, best_match, best_distance))
            unmatched_pred.remove(best_match)
            unmatched_gt.remove(gt)
    
    return {
        'matches': matches,
        'false_positives': unmatched_pred,
        'false_negatives': unmatched_gt,
        'precision': len(matches) / len(pred_events) if pred_events else 0,
        'recall': len(matches) / len(gt_events) if gt_events else 0
    }
```

### 8.3 Metrics Endpoint

```cpp
// In src/metrics/metrics_server.cpp
#include <prometheus/exposer.h>
#include <prometheus/registry.h>

class MetricsServer {
public:
    MetricsServer(const std::string& bind_address) {
        exposer_ = std::make_unique<prometheus::Exposer>(bind_address);
        registry_ = std::make_shared<prometheus::Registry>();
        exposer_->RegisterCollectable(registry_);
        
        // Define metrics
        auto& fps_gauge = prometheus::BuildGauge()
            .Name("argus_fps")
            .Help("Current frames per second")
            .Register(*registry_);
        fps_ = &fps_gauge.Add({});
        
        auto& latency_hist = prometheus::BuildHistogram()
            .Name("argus_latency_ms")
            .Help("End-to-end latency")
            .Register(*registry_);
        latency_ = &latency_hist.Add({}, {50, 100, 150, 200, 300});
    }
    
    void update_fps(double fps) { fps_->Set(fps); }
    void record_latency(double ms) { latency_->Observe(ms); }
};
```

**Usage:**

```bash
# Start metrics server
./attention_demo --metrics-port 9090

# Query
curl http://localhost:9090/metrics
# argus_fps 24.5
# argus_latency_ms{quantile="0.95"} 165.3
```

### 8.4 Overlay Validation

```cpp
void draw_debug_overlay(cv::Mat& frame, 
                        const std::vector<Track>& tracks,
                        const ROI& roi) {
    // Draw ROI boundary
    cv::rectangle(frame, roi, cv::Scalar(0,255,0), 2);
    
    // Draw hysteresis margin (dashed)
    int margin = 8;
    cv::rectangle(frame,
                  cv::Rect(roi.x - margin, roi.y - margin, 
                           roi.width + 2*margin, roi.height + 2*margin),
                  cv::Scalar(0,255,255), 1);
    
    // Draw tracks
    for (const auto& track : tracks) {
        cv::rectangle(frame, track.bbox, cv::Scalar(255,0,0), 2);
        
        // Track ID
        std::string label = "ID:" + std::to_string(track.id);
        cv::putText(frame, label, ...);
        
        // Event flags
        if (track.entered_this_frame) {
            cv::putText(frame, "E", center, ...);  // Green
        }
        if (track.exited_this_frame) {
            cv::putText(frame, "X", center, ...);  // Red
        }
        
        // Gaze indicator
        if (track.gaze_active) {
            cv::circle(frame, center, 20, cv::Scalar(0,255,255), -1);
        }
    }
}
```

---

## 9. Debug Playbook

### 9.1 "Enter/Exit Events Don't Fire"

**Symptoms:**

- Person crosses boundary but no event published
- MQTT log missing enter/exit

**Diagnosis:**

1. **Enable debug logging:**

```json
{"log_level": "debug", "debug_modules": ["zone_logic", "tracker"]}
```

2. **Check internal state:**

```cpp
LG_DEBUG("Track %d: inside=%d, frames_inside=%d, emitted_enter=%d",
         track.id, track.inside_roi, track.frames_inside, 
         track.emitted_enter);
```

3. **Visualize track center vs border:**

```bash
grep "Track center" /storage/sd/logs/gaze.log > /tmp/centers.csv
python3 plot_tracks.py --csv /tmp/centers.csv --roi 192,144,448,336
```

4. **Check debounce & hysteresis:**

   - Verify debounce_frames not too high
   - Verify hysteresis_margin appropriate for scene

5. **Check tracker continuity:**

```bash
grep "ID switch" /storage/sd/logs/gaze.log
```

**Common Causes:**

- ❌ Debounce frames too high (person crosses quickly)
- ❌ Hysteresis margin too large
- ❌ Tracker ID switches at boundary (resets FSM)
- ❌ ROI coordinates in different scale than bbox
- ❌ Using bbox center instead of footpoint

### 9.2 "Gaze Always False"

**Symptoms:**

- RetinaFace detects faces but gaze never turns ON

**Diagnosis:**

1. **Check face-to-person association:**

```cpp
LG_DEBUG("Person bbox: (%d,%d,%d,%d), Face bbox: (%d,%d,%d,%d), IoU: %.2f",
         person.x, person.y, person.width, person.height,
         face.x, face.y, face.width, face.height, iou);
```

2. **Check gaze confidence:**

```cpp
LG_DEBUG("Gaze: confidence=%.2f (threshold=%.2f), frames=%d/%d",
         confidence, threshold, consecutive_frames, required_frames);
```

3. **Visual validation:**

   - Enable overlay
   - Check if faces detected
   - Verify faces inside person bboxes

**Common Causes:**

- ❌ Face bbox not overlapping person bbox
- ❌ Fixation frames too high (person doesn't look long enough)
- ❌ Confidence threshold too high

### 9.3 "ID Switches Too Frequent"

**Symptoms:**

- Track IDs change for same person
- Counting inaccurate (multiple enters)

**Diagnosis:**

1. **Lower IoU threshold:**

```json
{"tracker_config": {"iou_threshold": 0.3}}  // Was 0.5
```

2. __Increase max_age:__

```json
{"tracker_config": {"max_age_frames": 60}}  // Was 30
```

3. **Visualize tracks:**

```bash
python3 plot_track_history.py --log /storage/sd/logs/gaze.log
```

**Common Causes:**

- ❌ IoU threshold too high
- ❌ max_age too low (expires during occlusion)
- ❌ Detection confidence fluctuates

### 9.4 "FPS Drops Below Target"

**Symptoms:**

- FPS < 20 sustained
- Latency increases

**Diagnosis:**

1. **Profile inference:**

```cpp
auto start = steady_clock::now();
inference_runner.run(frame);
auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start);
LG_DEBUG("Inference: %ld ms", elapsed.count());
```

2. **Check CPU/NPU:**

```bash
top -b -n 1 | grep attention_demo
cat /sys/class/thermal/thermal_zone0/temp
```

3. **Check queue depths:**

```cpp
LG_DEBUG("Queues - capture: %zu, face: %zu, yolo: %zu",
         capture_q.size(), face_q.size(), yolo_q.size());
```

**Common Causes:**

- ❌ Inference too slow (model too large)
- ❌ Frame queues backing up
- ❌ Memory leak causing swapping
- ❌ Thermal throttling

---

## 10. Automation

### pytest Framework

```python
# tests/conftest.py
import pytest

@pytest.fixture(scope="session")
def mqtt_broker():
    """Start local MQTT broker"""
    import subprocess
    proc = subprocess.Popen(['mosquitto', '-c', 'test_mosquitto.conf'])
    yield 'localhost:1883'
    proc.terminate()

@pytest.fixture
def mqtt_collector(mqtt_broker):
    collector = MQTTCollector(broker=mqtt_broker)
    collector.start()
    yield collector
    collector.stop()

# tests/test_component.py
@pytest.mark.parametrize("clip", [
    "test_data/clips/clip01_single_pass.mp4",
    "test_data/clips/clip02_hover_border.mp4",
])
def test_event_accuracy(clip, mqtt_collector, replay_harness):
    gt_events = load_ground_truth(f"{clip}.csv")
    replay_harness.run(clip, config='configs/config-test.json')
    mqtt_events = mqtt_collector.get_messages()
    
    results = evaluate_events(mqtt_events, gt_events, tolerance_ms=500)
    
    assert results['precision'] >= 0.95
    assert results['recall'] >= 0.95
    assert results['f1'] >= 0.97
```

### Commands

```bash
# Run all tests
pytest tests/ -v

# Specific category
pytest tests/test_component.py -v

# With coverage
pytest tests/ --cov=src --cov-report=html

# HTML report
pytest tests/ --html=report.html --self-contained-html
```

---

## 11. Release Criteria

### Alpha Release (Internal)

- ✅ Unit tests: 100% pass
- ✅ Component: ≥ 80% clips meet targets
- ✅ System: ≥ 60% scenarios pass
- ⚠️ Known issues documented

### Beta Release (Pilot)

- ✅ Unit tests: 100% pass
- ✅ Component: ≥ 90% clips meet targets
- ✅ System: ≥ 80% scenarios pass
- ✅ Soak: 4 hours stable

### Production Release

- ✅ Unit tests: 100% pass
- ✅ Component: 100% clips meet targets
- ✅ System: ≥ 95% scenarios pass
- ✅ Soak: 8+ hours stable
- ✅ All critical bugs fixed
- ✅ Documentation complete

---

## 12. Test Schedule

| Phase | Duration | Effort | Deliverables |
|-------|----------|--------|--------------|
| Setup | 1 week | 3 days | Clips, labels, harness |
| Unit | 1 week | 2 days | 50+ tests passing |
| Component | 2 weeks | 5 days | Automated on clips |
| System | 1 week | 3 days | Manual scenarios |
| Soak | 1 week | 1 day | 8-hour runs |
| Bug Fix | 2 weeks | 5 days | Address failures |
| Regression | 1 week | 2 days | Re-run all |
| **TOTAL** | **9 weeks** | **21 days** | **Production-ready** |

---

## 13. Resources

**Personnel:**

- 1 Test Engineer (lead)
- 1 Software Engineer
- 1 QA Technician

**Equipment:**

- 2x BrightSign HS145 (RK3568)
- 2x BrightSign XT5 (RK3588)
- 2x USB cameras
- 1x RTSP camera
- 1x Test display
- 1x Dev workstation

**Software:**

- CVAT (annotation)
- pytest + pytest-html
- Mosquitto (MQTT)
- Grafana + Prometheus
- FFmpeg

---

## 14. CI/CD Integration

```yaml
# .github/workflows/test.yml
name: Argus Tests

on:
  push:
    branches: [ main, develop ]

jobs:
  unit-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Build
        run: ./build-apps LS5
      - name: Test
        run: ./build_ls5/run_unit_tests

  component-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Download clips
        run: aws s3 sync s3://test-data/clips test_data/clips
      - name: Test
        run: pytest tests/test_component.py
```

### Nightly Tests

```bash
#!/bin/bash
DATE=$(date +%Y%m%d)
RESULTS="test_results/$DATE"

pytest tests/ --html=$RESULTS/report.html
./run_soak_test.sh --duration 14400
python3 generate_report.py --results-dir $RESULTS

cat $RESULTS/summary.html | mail -s "Results $DATE" team@example.com
```

---

## 15. Summary

This test strategy ensures production quality through:

✅ **4-layer pyramid** - Unit → Component → System → Soak  
✅ **Clear KPIs** - Measurable targets with thresholds  
✅ **Realistic scenarios** - Test conditions mirror deployment  
✅ **Automation** - Replay harness + pytest  
✅ **Debug tooling** - Playbook for rapid resolution  
✅ **CI/CD** - Catch regressions early

### Success Factors

1. Ground truth quality
2. Automation (replay harness)
3. Instrumentation (logging)
4. Continuous testing
5. Realistic test clips

### Next Steps

**Weeks 1-2**: Infrastructure (clips, labels, harness)  
**Weeks 3-4**: Unit + component tests  
**Weeks 5-6**: System tests  
**Weeks 7-8**: Bug fixes  
**Week 9**: Regression + validation

**Target**: Production-ready with ≥95% test coverage and all KPIs met.
