#!/bin/bash
set -euo pipefail

IMAGE_STREAM_SERVER_DIR="$1"
IMAGE_STREAM_SERVER_BINARY="$2"
CMAKE_BINARY_DIR="$3"

clone_or_update() {
  if [ ! -d "$IMAGE_STREAM_SERVER_DIR" ]; then
    echo 'Cloning bs-image-stream-server...'
    git clone --depth=1 https://github.com/brightsign/bs-image-stream-server.git "$IMAGE_STREAM_SERVER_DIR"
  else
    echo 'Repository already exists, updating...'
    git -C "$IMAGE_STREAM_SERVER_DIR" pull --rebase --autostash origin main || true
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
  cd "$IMAGE_STREAM_SERVER_DIR"

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

  echo 'Building image stream server for arm64...'
  echo "PWD: $(pwd)"
  make build-arm64

  if [ -f "$IMAGE_STREAM_SERVER_BINARY" ]; then
    echo 'Build completed; copying binary...'
    cp "$IMAGE_STREAM_SERVER_BINARY" "$CMAKE_BINARY_DIR/image-stream-server"
  else
    echo "Build failed - binary not found at $IMAGE_STREAM_SERVER_BINARY"
    exit 1
  fi
}

main "$@"
