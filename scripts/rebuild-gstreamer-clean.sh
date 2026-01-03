#!/bin/bash
# Force clean rebuild of GStreamer plugins (clears sstate cache for these packages)
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Detect container runtime
if command -v docker &>/dev/null && docker info &>/dev/null 2>&1; then
    CONTAINER_CMD="docker"
elif command -v podman &>/dev/null && podman info &>/dev/null 2>&1; then
    CONTAINER_CMD="podman"
else
    echo "ERROR: No container runtime available"
    exit 1
fi

echo "[INFO] Cleaning sstate cache for GStreamer packages..."
$CONTAINER_CMD run --rm \
    -v "$PROJECT_ROOT/brightsign-oe:/home/builder/bsoe" \
    -v "$PROJECT_ROOT/srv:/srv" \
    bsoe-build \
    bash -c "cd /home/builder/bsoe/build && MACHINE=cobra ./bsbb -c cleansstate gstreamer1.0-plugins-base && MACHINE=cobra ./bsbb -c cleansstate gstreamer1.0-plugins-good"

echo "[INFO] Rebuilding GStreamer plugins from scratch..."
$CONTAINER_CMD run --rm \
    -v "$PROJECT_ROOT/brightsign-oe:/home/builder/bsoe" \
    -v "$PROJECT_ROOT/srv:/srv" \
    bsoe-build \
    bash -c "cd /home/builder/bsoe/build && MACHINE=cobra ./bsbb gstreamer1.0-plugins-good gstreamer1.0-plugins-base"

echo "[OK] GStreamer plugins rebuilt. Now run ./scripts/build-gst-isomp4-plugin.sh to copy them."
