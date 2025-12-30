# Plan: Add argus-exporter to Extension

## Overview

This plan describes the steps to integrate `argus-exporter` from the private repository `git@github.com:BrightSign-Playground/argus-exporter.git` into the BrightSign NPU extension, following the same pattern as `bs-image-stream-server`.

## Prerequisites

Before starting implementation, verify:

1. **SSH Access to Private Repo**
   ```bash
   ssh -T git@github.com
   # Should show: "Hi <user>! You've successfully authenticated..."

   # Test clone access
   git ls-remote git@github.com:BrightSign-Playground/argus-exporter.git
   ```

2. **Understand argus-exporter Build Process**
   - Check if it has a Makefile with ARM64 target
   - Identify the output binary name and location
   - Note any build dependencies

## Questions to Resolve

Before implementation, clarify:

| Question | Options | Impact |
|----------|---------|--------|
| Build method | Makefile (`make build-arm64`) vs direct `go build` | Determines build script logic |
| Runtime behavior | Background daemon vs on-demand | Determines bsext_init integration |
| Configuration | Registry-based (port, enable/disable) vs static | Determines runtime configuration |
| Arguments | Command-line flags needed | Determines daemon startup command |
| Binary name | `argus-exporter` vs other | Affects all file paths |

---

## Implementation Steps

### Step 1: Create Build Script

**File:** `scripts/build_argus_exporter.sh`

```bash
#!/bin/bash
#
# Build script for argus-exporter
# Clones from private repo, builds for ARM64, copies to build directory
#
set -euo pipefail

ARGUS_EXPORTER_DIR=$1
ARGUS_EXPORTER_BINARY=$2
CMAKE_BINARY_DIR=$3

# Semantic version comparison: returns 0 (true) if $1 >= $2
ver_ge() {
  [ "$(printf '%s\n' "$1" "$2" | sort -V | head -n1)" = "$2" ]
}

clone_or_update() {
  if [ ! -d "$ARGUS_EXPORTER_DIR" ]; then
    echo 'Cloning argus-exporter...'
    git clone --depth=1 git@github.com:BrightSign-Playground/argus-exporter.git "$ARGUS_EXPORTER_DIR"
  else
    echo 'Repository already exists, updating...'
    git -C "$ARGUS_EXPORTER_DIR" pull --rebase --autostash origin main || true
  fi
}

main() {
  # Set GOTOOLCHAIN=local initially to prevent auto-download during version detection
  export GOTOOLCHAIN=local

  clone_or_update
  cd "$ARGUS_EXPORTER_DIR"

  # Detect local Go version
  GO_BIN="$(command -v go)"
  echo "Using go from PATH: $GO_BIN"
  GOV_FULL="$("$GO_BIN" version | awk '{print $3}' | sed 's/^go//')"
  GOV_SHORT="$(echo "$GOV_FULL" | awk -F. '{print $1"."$2}')"

  # Read requirements from go.mod
  REQ_TOOLCHAIN="$(awk '/^toolchain[[:space:]]+go/{print $2; exit}' go.mod | sed 's/^go//' || true)"
  REQ_GO_SHORT="$(awk '/^go[[:space:]]+[0-9]/{print $2; exit}' go.mod || true)"

  # Determine required full version
  if [ -n "$REQ_TOOLCHAIN" ]; then
    REQ_FULL="$REQ_TOOLCHAIN"
    REQ_SHORT="$(echo "$REQ_FULL" | awk -F. '{print $1"."$2}')"
  else
    REQ_FULL="${REQ_GO_SHORT}.0"
    REQ_SHORT="$REQ_GO_SHORT"
  fi

  echo "Local Go version: $GOV_FULL (short: $GOV_SHORT)"
  echo "Required Go version: $REQ_FULL (short: $REQ_SHORT)"

  # Handle Go version compatibility
  if ver_ge "$GOV_FULL" "$REQ_FULL"; then
    echo "Local Go ($GOV_FULL) >= required ($REQ_FULL); using local toolchain"
    export GOTOOLCHAIN=local
    if grep -q '^toolchain ' go.mod; then
      awk '!/^toolchain /{print}' go.mod > go.mod.tmp && mv go.mod.tmp go.mod
    fi
    "$GO_BIN" mod edit -go="$GOV_SHORT"
  else
    echo "Local Go ($GOV_FULL) < required ($REQ_FULL); enabling auto-download"
    export GOTOOLCHAIN=auto
    if [ -z "$REQ_TOOLCHAIN" ]; then
      printf 'toolchain go%s\n' "$REQ_FULL" >> go.mod
    fi
    "$GO_BIN" mod edit -go="$REQ_SHORT"
    "$GO_BIN" download "go${REQ_FULL}" || true
  fi

  echo "Running go mod tidy..."
  "$GO_BIN" mod tidy

  # Build for ARM64
  # Option A: If Makefile exists with build-arm64 target
  # echo 'Building argus-exporter for arm64...'
  # make build-arm64

  # Option B: Direct go build (use this if no Makefile)
  echo 'Building argus-exporter for arm64...'
  mkdir -p bin
  GOOS=linux GOARCH=arm64 CGO_ENABLED=0 "$GO_BIN" build -o bin/argus-exporter-arm64 .

  # Copy binary to build directory
  if [ -f "$ARGUS_EXPORTER_BINARY" ]; then
    echo 'Build completed; copying binary...'
    cp "$ARGUS_EXPORTER_BINARY" "$CMAKE_BINARY_DIR/argus-exporter"
  else
    echo "Build failed - binary not found at $ARGUS_EXPORTER_BINARY"
    exit 1
  fi
}

main
```

**Notes:**
- Uses SSH URL (`git@github.com:...`) for private repo access
- Includes Go toolchain version management from `build_image_server.sh`
- Build command may need adjustment based on actual repo structure

---

### Step 2: Update CMakeLists.txt

**Location:** After line ~363 (after `image_stream_server` target)

Add the following:

```cmake
# =============================================================================
# Clone and build the argus-exporter using external script
# =============================================================================
set(ARGUS_EXPORTER_DIR "${CMAKE_BINARY_DIR}/argus-exporter")
set(ARGUS_EXPORTER_BINARY "${ARGUS_EXPORTER_DIR}/bin/argus-exporter-arm64")

add_custom_target(argus_exporter ALL
  COMMAND ${CMAKE_COMMAND} -E echo "Cloning and building argus-exporter..."
  COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/build_argus_exporter.sh
          ${ARGUS_EXPORTER_DIR}
          ${ARGUS_EXPORTER_BINARY}
          ${CMAKE_BINARY_DIR}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Cloning and building argus-exporter"
  VERBATIM
)
```

**Location:** Update POST_BUILD commands (~line 366-381)

Add to both `if(HAVE_RGA)` and `else()` blocks:

```cmake
add_custom_command(TARGET attention_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_BINARY_DIR}/argus-exporter
          $<TARGET_FILE_DIR:attention_demo>/argus-exporter
          || ${CMAKE_COMMAND} -E true
  COMMENT "Copying argus-exporter to output directory"
)
```

**Location:** Update dependencies (~line 387)

```cmake
add_dependencies(attention_demo argus_exporter)
```

**Location:** Add install rule (~line 399)

```cmake
# Install the argus-exporter binary
install(FILES ${CMAKE_BINARY_DIR}/argus-exporter
        DESTINATION ./
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE
                   WORLD_READ WORLD_EXECUTE
        OPTIONAL)
```

---

### Step 3: Update Package Script

**File:** `package`
**Location:** After line ~263 (after image-stream-server copy)

```bash
# Copy argus-exporter binary
if [[ -f "$source_dir/argus-exporter" ]]; then
    cp "$source_dir/argus-exporter" "$package_soc_dir/"
    chmod +x "$package_soc_dir/argus-exporter"
else
    warn "argus-exporter not found in $source_dir"
fi
```

---

### Step 4: Update bsext_init (Runtime Integration)

**File:** `bsext_init`

#### 4a. Add PID file variable

**Location:** Near other PID file definitions (~line 20)

```bash
ARGUS_EXPORTER_PIDFILE=/var/run/bsext-argus-exporter.pid
```

#### 4b. Add run function

**Location:** After `run_stream_server()` function (~line 193)

```bash
run_argus_exporter() {
    echo "run_argus_exporter called with background=$1"
    local background=$1
    # Add additional arguments as needed
    # local port=$2

    SOC_NAME=$(get_soc_name)
    SOC_HOME=${SCRIPT_PATH}/${SOC_NAME}

    if [ "$background" = "true" ]; then
        echo "Starting argus-exporter as daemon"
        start-stop-daemon --start --background --make-pidfile \
                          --pidfile ${ARGUS_EXPORTER_PIDFILE} \
                          --exec ${SOC_HOME}/argus-exporter -- [ARGUMENTS]
    else
        ${SOC_HOME}/argus-exporter [ARGUMENTS]
    fi
}
```

#### 4c. Add to do_start()

**Location:** In `do_start()` function, after stream server start (~line 323)

```bash
# Start argus-exporter
# Option: Add registry-based enable/disable
# ARGUS_EXPORTER_ENABLED=$(registry networking argus-exporter-enabled)
# if [ "${ARGUS_EXPORTER_ENABLED}" != "0" ]; then
    run_argus_exporter true
# fi
```

#### 4d. Add to do_stop()

**Location:** In `do_stop()` function

```bash
# Stop argus-exporter
if [ -f "${ARGUS_EXPORTER_PIDFILE}" ]; then
    echo "Stopping argus-exporter..."
    start-stop-daemon --stop --pidfile ${ARGUS_EXPORTER_PIDFILE} || true
    rm -f ${ARGUS_EXPORTER_PIDFILE}
fi
```

---

## File Changes Summary

| File | Action | Lines Affected |
|------|--------|----------------|
| `scripts/build_argus_exporter.sh` | Create new | ~80 lines |
| `CMakeLists.txt` | Modify | ~20 lines added |
| `package` | Modify | ~6 lines added |
| `bsext_init` | Modify | ~25 lines added |

---

## Testing Plan

### 1. Build Test
```bash
# Clean build
rm -rf build && mkdir build && cd build

# Configure and build
cmake ..
make -j$(nproc)

# Verify binary exists
ls -la argus-exporter
file argus-exporter  # Should show ARM64 ELF
```

### 2. Package Test
```bash
# Run package script
./package

# Verify binary in staging
ls -la staging/RK3588/argus-exporter
```

### 3. Runtime Test (on device)
```bash
# After installing extension
/var/volatile/bsext/ext_npu_argus/RK3588/argus-exporter --help

# Check if running as daemon
ps aux | grep argus-exporter
cat /var/run/bsext-argus-exporter.pid
```

---

## Rollback Plan

If issues arise, revert changes:

1. Remove `scripts/build_argus_exporter.sh`
2. Revert `CMakeLists.txt` changes
3. Revert `package` changes
4. Revert `bsext_init` changes
5. Clean build directory: `rm -rf build`

---

## Open Items

- [ ] Confirm argus-exporter build method (Makefile vs go build)
- [ ] Determine runtime arguments
- [ ] Decide on registry-based configuration
- [ ] Verify SSH access works in build environment
- [ ] Test on all SOC variants (RK3588, RK3568, RK3576)
