#!/bin/bash
# quick_test_setup.sh - Test setup without requiring sudo

set -e

PROJECT_ROOT=$(pwd)
echo "=== Quick Test Setup (No Sudo Required) ==="
echo ""

# Check Google Test
echo "[1/5] Checking Google Test..."
if pkg-config --exists gtest 2>/dev/null || [ -f /usr/include/gtest/gtest.h ]; then
    echo "✅ Google Test found"
elif [ -d "/usr/src/gtest" ]; then
    echo "✅ Google Test source found at /usr/src/gtest"
else
    echo "⚠️  Google Test not found"
    echo ""
    echo "Please install manually:"
    echo "  sudo apt-get update"
    echo "  sudo apt-get install -y libgtest-dev cmake"
    echo ""
    echo "Then re-run this script."
    exit 1
fi

# Create tests directory
echo "[2/5] Setting up tests directory..."
mkdir -p tests

# Create simple test
echo "[3/5] Creating test files..."
cat > tests/test_simple.cpp << 'TESTEOF'
#include <gtest/gtest.h>

TEST(SimpleTest, BasicAssertion) {
    EXPECT_EQ(1 + 1, 2);
    EXPECT_TRUE(true);
}

TEST(SimpleTest, StringTest) {
    std::string hello = "Hello";
    EXPECT_EQ(hello, "Hello");
}
TESTEOF

# Create tests CMakeLists.txt
cat > tests/CMakeLists.txt << 'CMAKEEOF'
include_directories(${CMAKE_SOURCE_DIR}/include)

file(GLOB TEST_SOURCES "test_*.cpp")

add_executable(run_all_tests ${TEST_SOURCES})

target_link_libraries(run_all_tests
    gtest
    gtest_main
    pthread
)

enable_testing()
add_test(NAME AllUnitTests COMMAND run_all_tests)
CMAKEEOF

# Update root CMakeLists.txt
echo "[4/5] Configuring build..."
if ! grep -q "BUILD_TESTS" CMakeLists.txt 2>/dev/null; then
    cat >> CMakeLists.txt << 'ROOTCMAKE'

# Unit tests
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
ROOTCMAKE
fi

# Build
echo "[5/5] Building tests..."
mkdir -p build_tests
cd build_tests
cmake .. -DBUILD_TESTS=ON 2>&1 | grep -E "BUILD_TESTS|tests|Test" || true
make -j$(nproc)

cd "$PROJECT_ROOT"
echo ""
echo "✅ Setup complete!"
echo ""
echo "Run tests with:"
echo "  cd build_tests && ./tests/run_all_tests"
