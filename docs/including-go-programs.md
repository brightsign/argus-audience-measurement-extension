# Including Go Programs in the Extension

This document describes how to include Go programs (such as `bs-image-stream-server`) in the BrightSign NPU extension. The pattern demonstrated here can be used for adding other Go-based utilities.

## Overview

The extension integrates external Go programs through a multi-stage process:

1. **Clone** the repository during build
2. **Build** the Go binary for ARM64
3. **Install** the binary alongside other extension files
4. **Package** the binary for distribution
5. **Execute** at runtime via the init script

## Case Study: bs-image-stream-server

### 1. Build Script

The build script `scripts/build_image_server.sh` handles cloning and compilation:

```bash
#!/bin/bash
set -euo pipefail

IMAGE_STREAM_SERVER_DIR=$1
IMAGE_STREAM_SERVER_BINARY=$2
CMAKE_BINARY_DIR=$3

clone_or_update() {
  if [ ! -d "$IMAGE_STREAM_SERVER_DIR" ]; then
    echo 'Cloning bs-image-stream-server...'
    git clone --depth=1 https://github.com/brightsign/bs-image-stream-server.git "$IMAGE_STREAM_SERVER_DIR"
  else
    echo 'Repository already exists, updating...'
    git -C "$IMAGE_STREAM_SERVER_DIR" pull --rebase --autostash origin main || true
  fi
}
```

Key features:
- Uses shallow clone (`--depth=1`) for faster downloads
- Updates existing clones instead of re-cloning
- Accepts paths as parameters from CMake

#### Go Toolchain Management

The build script intelligently handles Go version compatibility:

```bash
# Read requirements from go.mod
REQ_TOOLCHAIN="$(awk '/^toolchain[[:space:]]+go/{print $2; exit}' go.mod || true)"
REQ_GO_SHORT="$(awk '/^go[[:space:]]+[0-9]/{print $2; exit}' go.mod || true)"

# If local Go is newer than required, use local toolchain
if ver_ge "$GOV_FULL" "$REQ_FULL"; then
  export GOTOOLCHAIN=local
  # Modify go.mod to use local Go version
  awk '!/^toolchain /{print}' go.mod > go.mod.tmp && mv go.mod.tmp go.mod
  "$GO_BIN" mod edit -go="$GOV_SHORT"
else
  export GOTOOLCHAIN=auto
  # Download required toolchain
  "$GO_BIN" toolchain download "go${REQ_FULL}" || true
fi
```

This ensures the build works regardless of the local Go version by:
- Using local Go if it's newer than required
- Automatically downloading the required version otherwise

#### Building and Copying

```bash
echo 'Building image stream server for arm64...'
make build-arm64

if [ -f "$IMAGE_STREAM_SERVER_BINARY" ]; then
  echo 'Build completed; copying binary...'
  cp "$IMAGE_STREAM_SERVER_BINARY" "$CMAKE_BINARY_DIR/image-stream-server"
fi
```

### 2. CMake Integration

In `CMakeLists.txt`, create a custom target that invokes the build script:

```cmake
# Define paths
set(IMAGE_STREAM_SERVER_DIR "${CMAKE_BINARY_DIR}/bs-image-stream-server")
set(IMAGE_STREAM_SERVER_BINARY "${IMAGE_STREAM_SERVER_DIR}/bin/image-stream-server-arm64")

# Create build target
add_custom_target(image_stream_server ALL
  COMMAND ${CMAKE_COMMAND} -E echo "Cloning and building bs-image-stream-server..."
  COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/build_image_server.sh
          ${IMAGE_STREAM_SERVER_DIR}
          ${IMAGE_STREAM_SERVER_BINARY}
          ${CMAKE_BINARY_DIR}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Cloning and building bs-image-stream-server"
  VERBATIM
)
```

#### Build Dependencies

Ensure the Go binary is built before the main target and copied to the output:

```cmake
# Ensure Go program builds before main target
add_dependencies(attention_demo image_stream_server)

# Copy binary after main target builds
add_custom_command(TARGET attention_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_BINARY_DIR}/image-stream-server
          $<TARGET_FILE_DIR:attention_demo>/image-stream-server
          || ${CMAKE_COMMAND} -E true
  COMMENT "Copying image-stream-server to output directory"
)
```

#### Installation

Install the binary with executable permissions:

```cmake
install(FILES ${CMAKE_BINARY_DIR}/image-stream-server
        DESTINATION ./
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE
                   WORLD_READ WORLD_EXECUTE
        OPTIONAL)
```

### 3. Packaging

The `package` script copies binaries to SOC-specific directories:

```bash
# Copy image stream server binary
if [[ -f "$source_dir/image-stream-server" ]]; then
    cp "$source_dir/image-stream-server" "$package_soc_dir/"
    chmod +x "$package_soc_dir/image-stream-server"
else
    warn "image-stream-server not found in $source_dir"
fi
```

Final package structure:
```
argus-ext-<timestamp>.zip
├── RK3588/
│   ├── attention_demo
│   ├── image-stream-server
│   └── ...
├── RK3568/
│   └── ...
└── RK3576/
    └── ...
```

### 4. Runtime Execution

The `bsext_init` script starts the Go program as a daemon:

```bash
run_stream_server() {
    local background=$1
    local port=$2

    SOC_NAME=$(get_soc_name)
    SOC_HOME=${SCRIPT_PATH}/${SOC_NAME}

    if [ "$background" = "true" ]; then
        start-stop-daemon --start --background --make-pidfile \
                          --pidfile ${STREAM_SERVER_PIDFILE} \
                          --exec ${SOC_HOME}/image-stream-server -- -port ${port}
    else
        ${SOC_HOME}/image-stream-server -port ${port}
    fi
}
```

Configuration via registry:
```bash
# Get port from registry (default: 20200)
STREAM_SERVER_PORT=20200
reg_stream_server_port=$(registry networking bs-image-stream-server-port)
if [ -n "${reg_stream_server_port}" ]; then
    STREAM_SERVER_PORT=${reg_stream_server_port}
fi

# Disable by setting port to 0
if [ "${STREAM_SERVER_PORT}" = "0" ]; then
    echo "Image stream server is disabled"
    return
fi

run_stream_server true ${STREAM_SERVER_PORT}
```

## Adding a New Go Program

To add a new Go program to the extension, follow these steps:

### Step 1: Create Build Script

Create `scripts/build_<program>.sh`:

```bash
#!/bin/bash
set -euo pipefail

PROGRAM_DIR=$1
PROGRAM_BINARY=$2
CMAKE_BINARY_DIR=$3

# Clone or update
if [ ! -d "$PROGRAM_DIR" ]; then
    git clone --depth=1 https://github.com/org/repo.git "$PROGRAM_DIR"
else
    git -C "$PROGRAM_DIR" pull --rebase --autostash origin main || true
fi

cd "$PROGRAM_DIR"

# Build for ARM64
GOOS=linux GOARCH=arm64 go build -o bin/program-arm64 .

# Copy to build directory
cp bin/program-arm64 "$CMAKE_BINARY_DIR/program"
```

### Step 2: Add CMake Target

In `CMakeLists.txt`:

```cmake
set(PROGRAM_DIR "${CMAKE_BINARY_DIR}/program-repo")
set(PROGRAM_BINARY "${PROGRAM_DIR}/bin/program-arm64")

add_custom_target(my_go_program ALL
  COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/build_<program>.sh
          ${PROGRAM_DIR} ${PROGRAM_BINARY} ${CMAKE_BINARY_DIR}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Building Go program"
  VERBATIM
)

add_dependencies(attention_demo my_go_program)

add_custom_command(TARGET attention_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_BINARY_DIR}/program
          $<TARGET_FILE_DIR:attention_demo>/program
          || ${CMAKE_COMMAND} -E true
)

install(FILES ${CMAKE_BINARY_DIR}/program
        DESTINATION ./
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE
                   WORLD_READ WORLD_EXECUTE
        OPTIONAL)
```

### Step 3: Update Package Script

In `package`, add:

```bash
if [[ -f "$source_dir/program" ]]; then
    cp "$source_dir/program" "$package_soc_dir/"
    chmod +x "$package_soc_dir/program"
fi
```

### Step 4: Add Runtime Execution

In `bsext_init`, add startup logic:

```bash
run_my_program() {
    local background=$1
    SOC_HOME=${SCRIPT_PATH}/$(get_soc_name)

    if [ "$background" = "true" ]; then
        start-stop-daemon --start --background --make-pidfile \
                          --pidfile /var/run/my-program.pid \
                          --exec ${SOC_HOME}/program -- [arguments]
    else
        ${SOC_HOME}/program [arguments]
    fi
}
```

Add to `do_start()`:
```bash
run_my_program true
```

Add to `do_stop()`:
```bash
if [ -f /var/run/my-program.pid ]; then
    start-stop-daemon --stop --pidfile /var/run/my-program.pid || true
    rm -f /var/run/my-program.pid
fi
```

## Build Requirements

- **Go compiler** (version compatible with the Go program's go.mod)
- **make** (if the Go project uses a Makefile)
- **git** for cloning repositories
- **bash** for build scripts

## File Locations

| Stage | Location |
|-------|----------|
| Source clone | `${CMAKE_BINARY_DIR}/<repo-name>/` |
| Build output | `${CMAKE_BINARY_DIR}/<binary>` |
| Install | `install/<SOC>/<binary>` |
| Package | `staging/<SOC>/<binary>` |
| Runtime | `/var/volatile/bsext/ext_npu_argus/<SOC>/<binary>` |

## Troubleshooting

### Go Version Mismatch

If the build fails with Go version errors, the build script should handle this automatically. If not:

```bash
# Check local Go version
go version

# Check required version
cat ${CMAKE_BINARY_DIR}/repo/go.mod | grep -E "^go |^toolchain"
```

### Binary Not Found

If the binary isn't copied correctly:

1. Check the build script completed successfully
2. Verify the binary path matches what CMake expects
3. Check the `make build-arm64` target exists in the Go project

### Runtime Failures

If the program fails to start:

1. Check logs: `journalctl -u bsext`
2. Verify the binary is executable: `ls -la /var/volatile/bsext/ext_npu_argus/<SOC>/`
3. Test manually: `/var/volatile/bsext/ext_npu_argus/<SOC>/program --help`
