#!/bin/bash
set -euo pipefail

ARGUS_EXPORTER_DIR="$1"
ARGUS_EXPORTER_BINARY="$2"
CMAKE_BINARY_DIR="$3"

# Commit tracking file for incremental builds
COMMIT_FILE="$CMAKE_BINARY_DIR/.argus-exporter-commit"

clone_or_update() {
  if [ ! -d "$ARGUS_EXPORTER_DIR" ]; then
    echo 'Cloning argus-exporter...'
    git clone git@github.com:BrightSign-Playground/argus-exporter.git "$ARGUS_EXPORTER_DIR"
  elif [ "${FORCE_UPDATE:-0}" = "1" ]; then
    echo 'FORCE_UPDATE set, pulling latest...'
    git -C "$ARGUS_EXPORTER_DIR" pull --rebase --autostash origin main || true
  else
    echo 'Repository exists, skipping git pull (set FORCE_UPDATE=1 to update)'
  fi
}

# Check if build can be skipped (binary exists and commit unchanged)
skip_if_unchanged() {
  local current_commit
  current_commit=$(git -C "$ARGUS_EXPORTER_DIR" rev-parse HEAD 2>/dev/null || echo "")

  if [ -z "$current_commit" ]; then
    return 1  # Can't determine commit, rebuild
  fi

  if [ -f "$COMMIT_FILE" ] && [ -f "$CMAKE_BINARY_DIR/argus-exporter" ]; then
    local last_commit
    last_commit=$(cat "$COMMIT_FILE")
    if [ "$last_commit" = "$current_commit" ]; then
      echo "[argus-exporter] No changes since last build (commit ${current_commit:0:8}), skipping..."
      return 0  # Skip build
    fi
  fi
  return 1  # Rebuild needed
}

# Save commit hash after successful build
save_commit() {
  local current_commit
  current_commit=$(git -C "$ARGUS_EXPORTER_DIR" rev-parse HEAD 2>/dev/null || echo "")
  if [ -n "$current_commit" ]; then
    echo "$current_commit" > "$COMMIT_FILE"
    echo "[argus-exporter] Saved build commit: ${current_commit:0:8}"
  fi
}

ver_ge() { # return 0 if $1 >= $2 (X.Y[.Z])
  a=$(echo "$1" | awk -F. '{printf "%03d.%03d.%03d", $1,$2,($3==""?0:$3)}')
  b=$(echo "$2" | awk -F. '{printf "%03d.%03d.%03d", $1,$2,($3==""?0:$3)}')
  [[ "$a" > "$b" || "$a" == "$b" ]]
}

main() {
  # Set GOTOOLCHAIN=local initially to prevent auto-download during version detection
  export GOTOOLCHAIN=local

  clone_or_update

  # Check if we can skip the build (commit unchanged)
  if skip_if_unchanged; then
    exit 0
  fi

  cd "$ARGUS_EXPORTER_DIR"

  # Local go version (full & X.Y)
  GO_BIN="$(command -v go)"
  echo "Using go from PATH: $GO_BIN"
  GOV_FULL="$("$GO_BIN" version | awk '{print $3}' | sed 's/^go//')"   # e.g. 1.22.2
  GOV_SHORT="$(echo "$GOV_FULL" | awk -F. '{print $1"."$2}')"         # e.g. 1.22
  echo "Local Go: full=$GOV_FULL short=$GOV_SHORT"

  # Read requirements from go.mod
  REQ_TOOLCHAIN="$(awk '/^toolchain[[:space:]]+go/{print $2; exit}' go.mod || true)"  # e.g. go1.25.0
  REQ_GO_SHORT="$(awk '/^go[[:space:]]+[0-9]/{print $2; exit}' go.mod || true)"       # e.g. 1.25

  if [ -n "$REQ_TOOLCHAIN" ]; then
    REQ_FULL="${REQ_TOOLCHAIN#go}"                                 # 1.25.0
    REQ_SHORT="$(echo "$REQ_FULL" | awk -F. '{print $1"."$2}')"    # 1.25
  else
    REQ_FULL="$REQ_GO_SHORT"                                       # 1.25
    REQ_SHORT="$REQ_GO_SHORT"                                      # 1.25
  fi

  echo "Repo requires: full=$REQ_FULL short=$REQ_SHORT (toolchain=${REQ_TOOLCHAIN:-none})"

  if ver_ge "$GOV_FULL" "$REQ_FULL"; then
    echo "Local Go is new enough; using local toolchain."
    export GOTOOLCHAIN=local
    # remove toolchain line if present; set go X.Y to our local X.Y
    if grep -q '^toolchain ' go.mod; then
      echo 'Removing toolchain directive from go.mod'
      awk '!/^toolchain /{print}' go.mod > go.mod.tmp && mv go.mod.tmp go.mod
    fi
    "$GO_BIN" mod edit -go="$GOV_SHORT"
  else
    echo "Local Go ($GOV_FULL) is older than required ($REQ_FULL); enabling toolchain auto-download."
    export GOTOOLCHAIN=auto
    # ensure toolchain line is present (with a full version, e.g. go1.25.0)
    if [ -z "$REQ_TOOLCHAIN" ]; then
      echo "Adding toolchain go${REQ_FULL} to go.mod"
      printf 'toolchain go%s\n' "$REQ_FULL" >> go.mod
    fi
    # set language version to the repo short requirement
    "$GO_BIN" mod edit -go="$REQ_SHORT"
    # (optional) prefetch the toolchain to avoid first-build latency
    "$GO_BIN" toolchain download "go${REQ_FULL}" || true
  fi

  echo "Running go mod tidy..."
  "$GO_BIN" mod tidy

  echo 'Building argus-exporter for arm64...'
  echo "PWD: $(pwd)"

  # Build from cmd/argus-exporter (standard Go project layout)
  # Output to repo root as argus-exporter-arm64
  GOOS=linux GOARCH=arm64 CGO_ENABLED=0 "$GO_BIN" build -o argus-exporter-arm64 ./cmd/argus-exporter

  if [ -f "$ARGUS_EXPORTER_BINARY" ]; then
    echo 'Build completed; copying binary...'
    cp "$ARGUS_EXPORTER_BINARY" "$CMAKE_BINARY_DIR/argus-exporter"
    # Save commit hash for future incremental build checks
    save_commit
  else
    echo "Build failed - binary not found at $ARGUS_EXPORTER_BINARY"
    exit 1
  fi
}

main "$@"
