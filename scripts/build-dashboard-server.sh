#!/usr/bin/env bash
set -euo pipefail

# Builds the in-repo dashboard-server (pure Go, stdlib only -> trivial cross-compile)
# and copies the binary next to the other player binaries. Invoked by the CMake
# `dashboard_server` target (GOARCH=arm64 for the player) and by `make
# dashboard-server-host` (host arch) for a quick compile check.
#
# Usage: build-dashboard-server.sh <src_dir> <out_binary> <copy_dir> [GOARCH]

SRC_DIR="${1:?usage: build-dashboard-server.sh <src_dir> <out_binary> <copy_dir> [GOARCH]}"
OUT_BIN="${2:?missing out_binary}"
COPY_DIR="${3:?missing copy_dir}"
GOARCH_IN="${4:-arm64}"

GO_BIN="$(command -v go)" || { echo "ERROR: go not found on PATH" >&2; exit 1; }
# stdlib-only module pinned to go 1.22; use the local toolchain, no auto-download.
export GOTOOLCHAIN=local CGO_ENABLED=0 GOOS=linux GOARCH="${GOARCH_IN}"

echo "Building dashboard-server (GOOS=${GOOS} GOARCH=${GOARCH})..."
( cd "${SRC_DIR}" && "${GO_BIN}" build -trimpath -o "${OUT_BIN}" . )
mkdir -p "${COPY_DIR}"
cp "${OUT_BIN}" "${COPY_DIR}/dashboard-server"
echo "dashboard-server built: ${OUT_BIN}"
