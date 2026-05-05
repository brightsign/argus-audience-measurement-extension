#!/usr/bin/env bash
# stream_as_rtsp.sh
#
# Streams the vest-overlay test video as an RTSP source on localhost:8554/live
# using mediamtx (RTSP server) + ffmpeg (publisher).
#
# Architecture:
#   ffmpeg -re -stream_loop -1  →  RTSP push  →  mediamtx  →  RTSP pull  →  Argus / ffplay
#
# The video must be encoded with keyframes every 30 frames (-g 30) so clients
# always join cleanly.  create_vest_overlay_video.py already does this.
#
# Usage:
#   bash scripts/stream_as_rtsp.sh [path/to/video-kf.mp4]
#
# Prerequisites (one-time):
#   curl -L https://github.com/bluenviron/mediamtx/releases/download/v1.9.3/mediamtx_v1.9.3_linux_amd64.tar.gz \
#        -o /tmp/mediamtx.tar.gz && tar -xzf /tmp/mediamtx.tar.gz -C /tmp mediamtx
#
# Ctrl-C stops everything.

set -euo pipefail
set +e   # mediamtx grep filter can exit non-zero; we handle failures explicitly
MTX_PID=""
FFM_PID=""
trap 'echo; echo "Stopping..."; [[ -n "${MTX_PID:-}" ]] && kill "$MTX_PID" 2>/dev/null; [[ -n "${FFM_PID:-}" ]] && kill "$FFM_PID" 2>/dev/null; exit 0' INT TERM

VIDEO="${1:-/home/sree/bs/argus_demo/000921246-vest-overlay-kf.mp4}"
MEDIAMTX="${MEDIAMTX_BIN:-/tmp/mediamtx}"
MEDIAMTX_CFG="${MEDIAMTX_CFG:-/tmp/mediamtx.yml}"
PORT=8554
STREAM_PATH="live"
RTSP_URL="rtsp://localhost:${PORT}/${STREAM_PATH}"

if [[ ! -f "$VIDEO" ]]; then
  echo "ERROR: Video not found: $VIDEO"
  echo "  Run first:  python3 scripts/create_vest_overlay_video.py"
  exit 1
fi

if [[ ! -x "$MEDIAMTX" ]]; then
  echo "ERROR: mediamtx not found at $MEDIAMTX"
  echo "  Download with:"
  echo "    curl -L https://github.com/bluenviron/mediamtx/releases/download/v1.9.3/mediamtx_v1.9.3_linux_amd64.tar.gz \\"
  echo "         -o /tmp/mediamtx.tar.gz"
  echo "    tar -xzf /tmp/mediamtx.tar.gz -C /tmp mediamtx"
  exit 1
fi

# Create mediamtx config if not already present —  allows publishing to any path
if [[ ! -f "$MEDIAMTX_CFG" ]]; then
  printf 'paths:\n  all_others:\n' > "$MEDIAMTX_CFG"
fi

echo "=================================================="
echo "  RTSP test stream (mediamtx + ffmpeg)"
echo "  Video : $VIDEO"
echo "  URL   : $RTSP_URL"
echo "  Press Ctrl-C to stop"
echo "=================================================="
echo ""
echo "  In argus-config.json set:"
echo '    "input_source": "rtsp"'
echo "    \"rtsp_url\": \"${RTSP_URL}\""
echo ""

# 1. Start mediamtx RTSP server (background), suppress normal log noise
"$MEDIAMTX" "$MEDIAMTX_CFG" 2>&1 | grep --line-buffered -v "INF\|WAR" &
MTX_PID=$!
sleep 1   # wait for port to open

# 2. Push the video via ffmpeg in an infinite loop.
#    Re-encode with ultrafast+zerolatency so every GOP is self-contained and
#    RTSP clients always decode from the first received keyframe.
echo "  [ffmpeg] Publishing → $RTSP_URL  (looping video)"
ffmpeg -hide_banner -loglevel warning \
  -re -stream_loop -1 \
  -i "$VIDEO" \
  -c:v libx264 -preset ultrafast -tune zerolatency \
  -g 30 -keyint_min 30 -sc_threshold 0 \
  -b:v 4M \
  -f rtsp -rtsp_transport tcp \
  "$RTSP_URL" &
FFM_PID=$!

echo "  Stream is live. Test with:"
echo "    ffplay -rtsp_transport tcp $RTSP_URL"
echo ""

wait "$FFM_PID"


