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
