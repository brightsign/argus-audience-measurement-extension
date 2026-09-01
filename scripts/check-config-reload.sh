#!/bin/bash
# Check Config Auto-Reload Feature Status
# Run this on the BrightSign device to verify deployment

echo "=========================================="
echo "Config Auto-Reload Feature Check"
echo "=========================================="
echo ""

# Check if wrapper script exists (new bsext_init)
echo "1. Checking for auto-restart wrapper..."
if [ -f "/tmp/attention_demo_wrapper.sh" ]; then
    echo "   ✅ Auto-restart wrapper found: /tmp/attention_demo_wrapper.sh"
    echo "   This indicates bsext_init with auto-restart support is installed"
else
    echo "   ❌ Auto-restart wrapper NOT found"
    echo "   You need to deploy the new package (argus-dev-1765378420.zip)"
fi
echo ""

# Check attention_demo binary timestamp
echo "2. Checking attention_demo binary..."
if [ -f "/var/volatile/bsext/ext_npu_argus/RK3568/attention_demo" ]; then
    BINARY_DATE=$(stat -c '%y' /var/volatile/bsext/ext_npu_argus/RK3568/attention_demo 2>/dev/null | cut -d' ' -f1)
    echo "   Binary date: $BINARY_DATE"
    echo "   (Should be 2025-12-10 or later for config monitor feature)"
else
    echo "   ❌ Binary not found"
fi
echo ""

# Check logs for ConfigMonitor messages
echo "3. Checking logs for ConfigMonitor..."
if grep -q "ConfigMonitor" /storage/sd/logs/gaze.log 2>/dev/null; then
    echo "   ✅ ConfigMonitor messages found in logs"
    echo "   Last ConfigMonitor message:"
    grep "ConfigMonitor" /storage/sd/logs/gaze.log 2>/dev/null | tail -1
else
    echo "   ❌ No ConfigMonitor messages in logs"
    echo "   This means the running binary doesn't have config monitor feature"
fi
echo ""

# Check running process
echo "4. Checking running processes..."
ATTENTION_PID=$(pgrep -f "attention_demo" | head -1)
if [ -n "$ATTENTION_PID" ]; then
    echo "   attention_demo PID: $ATTENTION_PID"
    WRAPPER_PID=$(pgrep -f "attention_demo_wrapper" | head -1)
    if [ -n "$WRAPPER_PID" ]; then
        echo "   ✅ Wrapper script PID: $WRAPPER_PID (auto-restart enabled)"
    else
        echo "   ⚠️  No wrapper script running (old version or manual start)"
    fi
else
    echo "   ❌ attention_demo not running"
fi
echo ""

# Check bsext_init for auto-restart code
echo "5. Checking bsext_init for auto-restart support..."
if grep -q "exit code 42" /var/volatile/bsext/ext_npu_argus/bsext_init 2>/dev/null; then
    echo "   ✅ bsext_init has auto-restart support (exit code 42 handling)"
else
    echo "   ❌ bsext_init does NOT have auto-restart support"
    echo "   You need to deploy the new bsext_init script"
fi
echo ""

# Summary
echo "=========================================="
echo "Summary:"
echo "=========================================="

HAS_WRAPPER=$([ -f "/tmp/attention_demo_wrapper.sh" ] && echo "yes" || echo "no")
HAS_MONITOR=$(grep -q "ConfigMonitor" /storage/sd/logs/gaze.log 2>/dev/null && echo "yes" || echo "no")
HAS_RESTART=$(grep -q "exit code 42" /var/volatile/bsext/ext_npu_argus/bsext_init 2>/dev/null && echo "yes" || echo "no")

if [ "$HAS_WRAPPER" = "yes" ] && [ "$HAS_MONITOR" = "yes" ] && [ "$HAS_RESTART" = "yes" ]; then
    echo "✅ Config auto-reload is FULLY FUNCTIONAL"
    echo ""
    echo "Test it:"
    echo "  1. vi /storage/sd/configs/argus-config.json"
    echo "  2. Make any change, save"
    echo "  3. tail -f /storage/sd/logs/gaze.log | grep -E 'CONFIG|Starting'"
    echo "  4. Should see restart within 5-13 seconds"
elif [ "$HAS_RESTART" = "yes" ] && [ "$HAS_WRAPPER" = "no" ]; then
    echo "⚠️  Config auto-reload is PARTIALLY INSTALLED"
    echo ""
    echo "The bsext_init script is updated, but not started with new version"
    echo ""
    echo "Fix:"
    echo "  1. /var/volatile/bsext/ext_npu_argus/bsext_init stop"
    echo "  2. /var/volatile/bsext/ext_npu_argus/bsext_init start"
else
    echo "❌ Config auto-reload is NOT INSTALLED"
    echo ""
    echo "You need to deploy the new package:"
    echo ""
    echo "  1. Stop: /var/volatile/bsext/ext_npu_argus/bsext_init stop"
    echo "  2. Extract: cd /var/volatile/bsext/ext_npu_argus && unzip -o /storage/sd/argus-dev-1765378420.zip"
    echo "  3. Start: /var/volatile/bsext/ext_npu_argus/bsext_init start"
    echo "  4. Test: vi /storage/sd/configs/argus-config.json (make a change)"
fi

echo ""
