#!/bin/bash
# Test if MQTT messages are being published from the player

PLAYER_IP="192.168.0.107"

echo "=========================================="
echo "🧪 Testing MQTT Publishing"
echo "=========================================="
echo ""

echo "From your Ubuntu machine, this should show messages:"
echo ""
echo "mosquitto_sub -h $PLAYER_IP -p 1883 -t 'bs/argus/analytics' -v"
echo ""
echo "Let me test for 10 seconds..."
echo ""

timeout 10 mosquitto_sub -h $PLAYER_IP -p 1883 -t 'bs/argus/analytics' -v || echo "No messages received in 10 seconds"

echo ""
echo "=========================================="
echo "Also testing all topics (#):"
echo "=========================================="
echo ""

timeout 5 mosquitto_sub -h $PLAYER_IP -p 1883 -t '#' -v || echo "No messages on any topic"

echo ""
echo ""
echo "If NO messages appeared above, the issue is:"
echo "  - attention_demo is NOT publishing to MQTT"
echo "  - Check if attention_demo is actually running"
echo "  - Check if it connected to the broker"
echo ""
echo "Run on player:"
echo "  ps | grep attention_demo"
echo "  cat /storage/sd/logs/gaze.log | tail -50"
echo ""
