# Unit Tests - Manual Setup Guide

## Current Status

✅ Test infrastructure is set up and working  
✅ Build system configured  
✅ Initial tests passing (2 simple tests)

---

## First Time Setup (Step-by-Step Manual Instructions)

### Prerequisites Check

Before starting, verify you have the required tools:

```bash
# Check if Google Test is installed
dpkg -l | grep libgtest-dev

# Check CMake version (need >= 3.4)
cmake --version

# Check compiler
g++ --version
```

---

### Step 1: Install Dependencies

If Google Test is not installed:

```bash
sudo apt-get update
sudo apt-get install -y libgtest-dev cmake build-essential
```

**Expected output**: Package installation messages, no errors.

**Verify installation**:

```bash
ls /usr/include/gtest/gtest.h
# Should show: /usr/include/gtest/gtest.h
```

---

### Step 2: Navigate to Project Directory

```bash
cd /home/sree/bs/argus_demo/brightsign-npu-gaze-extension-ng
```

**Verify you're in the correct directory**:

```bash
pwd
# Should show: /home/sree/bs/argus_demo/brightsign-npu-gaze-extension-ng

ls tests/
# Should show: README.md, CMakeLists.txt, test_simple.cpp
```

---

### Step 3: Create Build Directory

```bash
mkdir -p build_tests
cd build_tests
```

**Verify**:

```bash
pwd
# Should show: .../brightsign-npu-gaze-extension-ng/build_tests
```

---

### Step 4: Configure Build with CMake

```bash
cmake .. -DBUILD_TESTS=ON
```

**Expected output**:

```yaml
-- The C compiler identification is GNU 13.3.0
-- The CXX compiler identification is GNU 13.3.0
...
-- Configuring done
-- Generating done
-- Build files have been written to: .../build_tests
```

__⚠️ If you see errors about OECORE_TARGET_SYSROOT__: This is normal, the build will continue.

---

### Step 5: Build the Tests

```bash
make run_all_tests -j4
```

**Expected output**:

```sh
[ 50%] Building CXX object tests/CMakeFiles/run_all_tests.dir/test_simple.cpp.o
[100%] Linking CXX executable run_all_tests
[100%] Built target run_all_tests
```

**Build time**: ~5-10 seconds on modern hardware.

---

### Step 6: Run the Tests

```bash
./tests/run_all_tests
```

**Expected output**:

```sh
Running main() from ./googletest/src/gtest_main.cc
[==========] Running 2 tests from 1 test suite.
[----------] 2 tests from SimpleTest
[ RUN      ] SimpleTest.BasicAssertion
[       OK ] SimpleTest.BasicAssertion (0 ms)
[ RUN      ] SimpleTest.StringTest
[       OK ] SimpleTest.StringTest (0 ms)
[----------] 2 tests from SimpleTest (0 ms total)
[  PASSED  ] 2 tests.
```

✅ **Success**: If you see `[  PASSED  ] 2 tests.`, your setup is complete!

---

### Step 7: Run with Color Output (Recommended)

```bash
./tests/run_all_tests --gtest_color=yes
```

This shows passed tests in green and failed tests in red.

---

## ✅ Setup Verification Checklist

- [ ] `build_tests/` directory exists
- [ ] `build_tests/tests/run_all_tests` executable exists
- [ ] Running `./tests/run_all_tests` shows `[  PASSED  ] 2 tests.`
- [ ] No error messages during build or run

**If all checks pass**: 🎉 **You're ready to develop tests!**

---

## Daily Workflow (After Initial Setup)

### Making Code Changes

```bash
# Navigate to build directory
cd /home/sree/bs/argus_demo/brightsign-npu-gaze-extension-ng/build_tests

# Rebuild (only recompiles changed files)
make -j4

# Run tests
./tests/run_all_tests --gtest_color=yes
```

**Quick one-liner**:

```bash
cd build_tests && make -j4 && ./tests/run_all_tests --gtest_color=yes
```

---

### Running Specific Tests

```bash
# Run all tests
./tests/run_all_tests

# Run only ZoneLogic tests
./tests/run_all_tests --gtest_filter=ZoneLogic*

# Run only Tracker tests
./tests/run_all_tests --gtest_filter=TrackerLogic*

# Run single specific test
./tests/run_all_tests --gtest_filter=ZoneLogicTest.SimpleEntry

# Run all tests with "Border" in the name
./tests/run_all_tests --gtest_filter=*Border*
```

---

### Debugging Failed Tests

```bash
# Run test with verbose output
./tests/run_all_tests --gtest_filter=MyTest --gtest_print_time=1

# Run test multiple times (catch flaky tests)
./tests/run_all_tests --gtest_repeat=10

# Shuffle test order (detect order dependencies)
./tests/run_all_tests --gtest_shuffle

# Run with debugger
gdb ./tests/run_all_tests
(gdb) run --gtest_filter=MyFailingTest
(gdb) bt    # Get backtrace on failure
```

---

### Clean Rebuild (When Things Go Wrong)

```bash
# From project root
cd /home/sree/bs/argus_demo/brightsign-npu-gaze-extension-ng

# Delete build directory
rm -rf build_tests

# Rebuild from scratch
mkdir build_tests && cd build_tests
cmake .. -DBUILD_TESTS=ON
make -j4

# Run tests
./tests/run_all_tests --gtest_color=yes
```

---

## Adding New Tests

### Create a New Test File

```bash
# From tests directory
cd /home/sree/bs/argus_demo/brightsign-npu-gaze-extension-ng/tests

# Create new test file
cat > test_myfeature.cpp << 'EOF'
#include <gtest/gtest.h>

TEST(MyFeature, BasicTest) {
    EXPECT_EQ(1 + 1, 2);
    EXPECT_TRUE(true);
}

TEST(MyFeature, AnotherTest) {
    int value = 42;
    EXPECT_GT(value, 0);
    EXPECT_LT(value, 100);
}
EOF
```

### Rebuild and Run

```bash
cd ../build_tests
make -j4
./tests/run_all_tests --gtest_filter=MyFeature*
```

---

## Common Issues & Solutions

### Issue: "gtest/gtest.h: No such file or directory"

**Solution**: Install Google Test

```bash
sudo apt-get install -y libgtest-dev
cd build_tests && rm -rf *
cmake .. -DBUILD_TESTS=ON && make -j4
```

---

### Issue: "unrecognized option '-Wpoison-system-directories'"

**Solution**: This was already fixed in CMakeLists.txt. If you still see it:

```bash
grep -A 3 "if(NOT BUILD_TESTS)" ../CMakeLists.txt
```

Should show the conditional check around the cross-compile flags.

---

### Issue: Tests compile but crash

**Solution**: Run with debugger

```bash
gdb ./tests/run_all_tests
(gdb) run
(gdb) bt
```

---

## Google Test Quick Reference

### Common Assertions

```cpp
// Equality
EXPECT_EQ(a, b);   // a == b
EXPECT_NE(a, b);   // a != b

// Comparison
EXPECT_LT(a, b);   // a < b
EXPECT_LE(a, b);   // a <= b
EXPECT_GT(a, b);   // a > b
EXPECT_GE(a, b);   // a >= b

// Boolean
EXPECT_TRUE(condition);
EXPECT_FALSE(condition);

// String
EXPECT_STREQ(str1, str2);  // C-strings
EXPECT_EQ(str1, str2);     // std::string

// Floating point
EXPECT_NEAR(val1, val2, tolerance);
```

__Note__: Use `ASSERT_*` to abort test on failure, `EXPECT_*` to continue.

---

## Test Files Structure

```ini
tests/
├── README.md              # This file
├── CMakeLists.txt         # Build configuration
├── test_simple.cpp        # ✅ Working (2 tests)
├── test_zone_logic.cpp    # ⏳ TODO
├── test_tracker.cpp       # ⏳ TODO
├── test_gaze_logic.cpp    # ⏳ TODO
├── test_direction.cpp     # ⏳ TODO
├── test_dwell.cpp         # ⏳ TODO
└── test_speed.cpp         # ⏳ TODO

build_tests/
└── tests/
    └── run_all_tests      # Test executable
```

---

## Quick Command Reference

```bash
# Build
cd build_tests
cmake .. -DBUILD_TESTS=ON
make run_all_tests -j4

# Run
./tests/run_all_tests --gtest_color=yes

# Filter
./tests/run_all_tests --gtest_filter=ZoneLogic*

# Clean rebuild
rm -rf build_tests && mkdir build_tests && cd build_tests
cmake .. -DBUILD_TESTS=ON && make -j4
```

---

## Resources

- **Full Test Strategy**: `docs/TEST-STRATEGY-COMPLETE.md`
- **Setup Success Log**: `TEST-SETUP-SUCCESS.md`
- **Google Test Docs**: https://google.github.io/googletest/

---

**Last Updated**: 2026-02-03  
**Status**: ✅ READY FOR DEVELOPMENT  
**Current**: 2/2 tests passing (100%)
