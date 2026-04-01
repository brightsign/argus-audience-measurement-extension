#!/bin/bash
# MQTT Test Script for BrightSign Player
# This script helps test MQTT connectivity and message flow

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}============================================${NC}"
echo -e "${BLUE}BrightSign MQTT Test Script${NC}"
echo -e "${BLUE}============================================${NC}"
echo ""

# Configuration
PLAYER_IP="${1:-192.168.0.107}"  # Default IP, can be overridden
MQTT_PORT=1883
ANALYTICS_TOPIC="bs/argus/analytics"

echo -e "${YELLOW}Configuration:${NC}"
echo "  Player IP: $PLAYER_IP"
echo "  MQTT Port: $MQTT_PORT"
echo "  Analytics Topic: $ANALYTICS_TOPIC"
echo ""

# Test 1: Check if player is reachable
echo -e "${YELLOW}[TEST 1] Checking network connectivity...${NC}"
if ping -c 1 -W 2 $PLAYER_IP > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Player is reachable at $PLAYER_IP${NC}"
else
    echo -e "${RED}✗ Cannot reach player at $PLAYER_IP${NC}"
    echo "  Please check:"
    echo "    - Player IP address (current: $PLAYER_IP)"
    echo "    - Network connection"
    echo "    - Firewall settings"
    exit 1
fi
echo ""

# Test 2: Check if MQTT port is open
echo -e "${YELLOW}[TEST 2] Checking MQTT port...${NC}"
if timeout 2 bash -c "echo > /dev/tcp/$PLAYER_IP/$MQTT_PORT" 2>/dev/null; then
    echo -e "${GREEN}✓ MQTT port $MQTT_PORT is open${NC}"
else
    echo -e "${RED}✗ MQTT port $MQTT_PORT is not accessible${NC}"
    echo "  Possible issues:"
    echo "    - Mosquitto broker not running on player"
    echo "    - Port 1883 blocked by firewall"
    echo "    - Player not properly configured"
    echo ""
    echo "  On player, check:"
    echo "    ps | grep mosquitto"
    echo "    netstat -tlnp | grep 1883"
    exit 1
fi
echo ""

# Test 3: Test basic MQTT connection
echo -e "${YELLOW}[TEST 3] Testing MQTT connection...${NC}"
if timeout 5 mosquitto_sub -h $PLAYER_IP -p $MQTT_PORT -t test -C 1 > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Can connect to MQTT broker${NC}"
else
    echo -e "${RED}✗ Cannot connect to MQTT broker${NC}"
    echo "  Check mosquitto logs on player"
    exit 1
fi
echo ""

# Test 4: Publish test message
echo -e "${YELLOW}[TEST 4] Publishing test message...${NC}"
if mosquitto_pub -h $PLAYER_IP -p $MQTT_PORT -t "test/from/ubuntu" -m "Hello from Ubuntu $(date)" 2>/dev/null; then
    echo -e "${GREEN}✓ Successfully published test message${NC}"
else
    echo -e "${RED}✗ Failed to publish test message${NC}"
fi
echo ""

# Test 5: Subscribe to analytics topic
echo -e "${YELLOW}[TEST 5] Subscribing to analytics topic...${NC}"
echo -e "${BLUE}Listening for messages on '$ANALYTICS_TOPIC'${NC}"
echo -e "${BLUE}Press Ctrl+C to stop...${NC}"
echo ""
echo -e "${GREEN}Waiting for analytics messages:${NC}"
echo "-------------------------------------------"

# Subscribe with verbose output and timestamp
mosquitto_sub -h $PLAYER_IP -p $MQTT_PORT -t "$ANALYTICS_TOPIC" -v | while read -r line; do
    timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo -e "${GREEN}[$timestamp]${NC} $line"
done
