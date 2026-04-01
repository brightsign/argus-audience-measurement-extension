#!/bin/bash
# setup_and_run_tests.sh - Complete test setup and execution

set -e

PROJECT_ROOT=$(pwd)
echo "=== BrightSign NPU Gaze Extension - Test Setup ==="
echo "Project root: $PROJECT_ROOT"
echo ""

# Step 1: Check if tests directory exists
if [ ! -d "tests" ]; then
    echo "[1/6] Creating tests directory..."
    mkdir -p tests
    echo "✅ Tests directory created"
else
    echo "[1/6] Tests directory exists"
fi

# Step 2: Install Google Test if needed
echo "[2/6] Checking Google Test installation..."
if ! dpkg -l | grep -q libgtest-dev; then
    echo "Installing Google Test..."
    sudo apt-get update
    sudo apt-get install -y libgtest-dev cmake build-essential
    echo "✅ Google Test installed"
else
    echo "✅ Google Test already installed"
fi

# Step 3: Create a simple test file if none exists
if [ ! -f "tests/test_simple.cpp" ]; then
    echo "[3/6] Creating initial test file..."
    cat > tests/test_simple.cpp << 'TESTEOF'
#include <gtest/gtest.h>

// Simple sanity test
TEST(SimpleTest, BasicAssertion) {
    EXPECT_EQ(1 + 1, 2);
    EXPECT_TRUE(true);
    EXPECT_FALSE(false);
}

TEST(SimpleTest, StringComparison) {
    std::string hello = "Hello";
    EXPECT_EQ(hello, "Hello");
    EXPECT_NE(hello, "World");
}
TESTEOF
    echo "✅ Initial test file created"
else
    echo "[3/6] Test files exist"
fi

# Step 4: Create tests/CMakeLists.txt if it doesn't exist
if [ ! -f "tests/CMakeLists.txt" ]; then
    echo "[4/6] Creating tests/CMakeLists.txt..."
    cat > tests/CMakeLists.txt << 'CMAKEEOF'
# Include project headers
include_directories(${CMAKE_SOURCE_DIR}/include)

# Collect all test source files
file(GLOB TEST_SOURCES "test_*.cpp")

# Create test executable
add_executable(run_all_tests ${TEST_SOURCES})

# Link Google Test and other dependencies
target_link_libraries(run_all_tests
    gtest
    gtest_main
    pthread
)

# Register with CTest
enable_testing()
add_test(NAME AllUnitTests COMMAND run_all_tests)
CMAKEEOF
    echo "✅ tests/CMakeLists.txt created"
else
    echo "[4/6] tests/CMakeLists.txt exists"
fi

# Step 5: Update root CMakeLists.txt if needed
if ! grep -q "BUILD_TESTS" CMakeLists.txt; then
    echo "[5/6] Updating root CMakeLists.txt..."
    cat >> CMakeLists.txt << 'ROOTCMAKE'

# Unit tests
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
ROOTCMAKE
    echo "✅ Root CMakeLists.txt updated"
else
    echo "[5/6] Root CMakeLists.txt already configured for tests"
fi

# Step 6: Build tests
echo "[6/6] Building tests..."
mkdir -p build_tests
cd build_tests

echo "Running CMake..."
cmake .. -DBUILD_TESTS=ON

echo "Compiling..."
make -j$(nproc)

cd "$PROJECT_ROOT"

echo ""
echo "✅ Test setup complete!"
echo ""
echo "=== Running Tests ==="
cd build_tests
./tests/run_all_tests --gtest_color=yes

echo ""
echo "=== Next Steps ==="
echo "1. Tests are now set up and working"
echo "2. Add more test files to tests/ directory"
echo "3. Run tests anytime with:"
echo "   cd build_tests && ./tests/run_all_tests"
echo "4. Track results with:"
echo "   ./track_test_results.sh"
