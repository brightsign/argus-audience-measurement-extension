#!/bin/bash
# track_test_results.sh - Track test results with proper error handling

DATE=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="test_results"
PROJECT_ROOT=$(pwd)

# Create results directory
mkdir -p "$RESULTS_DIR"

# Check if build_tests exists
if [ ! -d "build_tests" ]; then
    echo "❌ Error: build_tests directory not found"
    echo ""
    echo "Please run setup first:"
    echo "  ./setup_and_run_tests.sh"
    echo ""
    exit 1
fi

# Check if test executable exists
if [ ! -f "build_tests/tests/run_all_tests" ]; then
    echo "❌ Error: Test executable not found"
    echo ""
    echo "Please build tests first:"
    echo "  cd build_tests"
    echo "  cmake .. -DBUILD_TESTS=ON"
    echo "  make -j4"
    echo ""
    exit 1
fi

echo "=== Running Tests at $(date) ==="
cd build_tests

# Run tests and capture output
./tests/run_all_tests --gtest_color=yes 2>&1 | tee "$PROJECT_ROOT/$RESULTS_DIR/run_$DATE.log"

# Check if tests passed
TEST_RESULT=${PIPESTATUS[0]}

# Extract summary
echo "" >> "$PROJECT_ROOT/$RESULTS_DIR/run_$DATE.log"
echo "=== Test Summary ===" >> "$PROJECT_ROOT/$RESULTS_DIR/run_$DATE.log"
grep -E "\[  PASSED  \]|\[  FAILED  \]" "$PROJECT_ROOT/$RESULTS_DIR/run_$DATE.log" || true

# Generate XML for later analysis
./tests/run_all_tests --gtest_output=xml:"$PROJECT_ROOT/$RESULTS_DIR/results_$DATE.xml" 2>/dev/null

cd "$PROJECT_ROOT"

echo ""
echo "Results saved to: $RESULTS_DIR/run_$DATE.log"
echo "XML report: $RESULTS_DIR/results_$DATE.xml"

# Show recent test history
echo ""
echo "=== Recent Test Runs ==="
ls -lt "$RESULTS_DIR"/run_*.log 2>/dev/null | head -5 || echo "No previous runs"

exit $TEST_RESULT
