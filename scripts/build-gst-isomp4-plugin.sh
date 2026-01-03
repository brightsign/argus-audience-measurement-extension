#!/bin/bash
# Build GStreamer plugins for MP4 file support
# - libgstisomp4.so: provides qtdemux for MP4/MOV demuxing (from gst-plugins-good)
# - libgstplayback.so: provides decodebin/uridecodebin (from gst-plugins-base)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info() { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Build GStreamer plugins for MP4 file support"
    echo ""
    echo "Options:"
    echo "  --isomp4-only    Build only libgstisomp4.so (qtdemux)"
    echo "  --playback-only  Build only libgstplayback.so (decodebin)"
    echo "  --all            Build both plugins (default)"
    echo "  -h, --help       Show this help"
    echo ""
}

BUILD_ISOMP4=true
BUILD_PLAYBACK=true

while [[ $# -gt 0 ]]; do
    case $1 in
        --isomp4-only) BUILD_ISOMP4=true; BUILD_PLAYBACK=false; shift ;;
        --playback-only) BUILD_ISOMP4=false; BUILD_PLAYBACK=true; shift ;;
        --all) BUILD_ISOMP4=true; BUILD_PLAYBACK=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1"; usage; exit 1 ;;
    esac
done

# Detect container runtime
CONTAINER_CMD=""
if command -v docker &>/dev/null && docker info &>/dev/null 2>&1; then
    CONTAINER_CMD="docker"
elif command -v podman &>/dev/null && podman info &>/dev/null 2>&1; then
    CONTAINER_CMD="podman"
else
    error "No container runtime (docker/podman) available"
fi
info "Using container runtime: $CONTAINER_CMD"

# Check prerequisites
if [[ ! -d "$PROJECT_ROOT/brightsign-oe" ]]; then
    error "brightsign-oe directory not found. Run runall.sh first to set up the build environment."
fi

if ! $CONTAINER_CMD images | grep -q "bsoe-build"; then
    error "bsoe-build container image not found. Run runall.sh first."
fi

# Target plugins
PLUGINS_TO_BUILD=()
if $BUILD_ISOMP4; then
    PLUGINS_TO_BUILD+=("libgstisomp4.so:gstreamer1.0-plugins-good")
fi
if $BUILD_PLAYBACK; then
    PLUGINS_TO_BUILD+=("libgstplayback.so:gstreamer1.0-plugins-base")
fi

if [[ ${#PLUGINS_TO_BUILD[@]} -eq 0 ]]; then
    error "No plugins selected to build"
fi

# Determine unique packages to build
PACKAGES_TO_BUILD=()
for entry in "${PLUGINS_TO_BUILD[@]}"; do
    pkg="${entry#*:}"
    if [[ ! " ${PACKAGES_TO_BUILD[*]} " =~ " ${pkg} " ]]; then
        PACKAGES_TO_BUILD+=("$pkg")
    fi
done

info "Plugins to build: ${PLUGINS_TO_BUILD[*]}"
info "Packages required: ${PACKAGES_TO_BUILD[*]}"

# Build packages in container using bsbb (the BrightSign build wrapper)
# bsbb sets oeroot and sources oe-init-build-env correctly
for pkg in "${PACKAGES_TO_BUILD[@]}"; do
    info "Building $pkg in container (this may take a while)..."
    $CONTAINER_CMD run --rm \
        -v "$PROJECT_ROOT/brightsign-oe:/home/builder/bsoe" \
        -v "$PROJECT_ROOT/srv:/srv" \
        bsoe-build \
        bash -c "cd /home/builder/bsoe/build && MACHINE=cobra ./bsbb $pkg"
done

# Copy function
copy_plugin() {
    local plugin_name="$1"

    info "Searching for $plugin_name..."
    local plugin_path=$(find "$PROJECT_ROOT/brightsign-oe/build/tmp-glibc" -name "$plugin_name" -path "*/aarch64*" 2>/dev/null | grep -v "\.debug" | head -1)

    if [[ -z "$plugin_path" ]]; then
        plugin_path=$(find "$PROJECT_ROOT/brightsign-oe/build" -name "$plugin_name" 2>/dev/null | grep -v "\.debug" | head -1)
    fi

    if [[ -z "$plugin_path" ]]; then
        warn "Could not find $plugin_name - skipping"
        return 1
    fi

    success "Found: $plugin_path"

    # Verify architecture
    if ! file "$plugin_path" | grep -q "aarch64"; then
        warn "$plugin_name may not be aarch64: $(file "$plugin_path")"
    fi

    # Copy to install directories
    local platforms=("RK3568" "RK3576" "RK3588")
    for platform in "${platforms[@]}"; do
        local dest_dir="$PROJECT_ROOT/install/$platform/lib/gstreamer-1.0"
        if [[ -d "$PROJECT_ROOT/install/$platform" ]]; then
            mkdir -p "$dest_dir"
            cp "$plugin_path" "$dest_dir/"
            chmod 644 "$dest_dir/$plugin_name"
            success "Installed $plugin_name for $platform"
        fi
    done

    # Copy to SDK
    local sdk_dir="$PROJECT_ROOT/sdk/sysroots/aarch64-oe-linux/usr/lib/gstreamer-1.0"
    if [[ -d "$sdk_dir" ]]; then
        cp "$plugin_path" "$sdk_dir/"
        success "Installed $plugin_name to SDK"
    fi

    return 0
}

# Find and copy plugins
installed_count=0
for entry in "${PLUGINS_TO_BUILD[@]}"; do
    plugin_name="${entry%%:*}"
    if copy_plugin "$plugin_name"; then
        ((installed_count++))
    fi
done

echo ""
if [[ $installed_count -eq ${#PLUGINS_TO_BUILD[@]} ]]; then
    success "All plugins built and installed!"
else
    warn "Some plugins failed to install ($installed_count/${#PLUGINS_TO_BUILD[@]})"
fi

echo ""
info "Next steps:"
echo "  1. Rebuild extension package: ./package"
echo "  2. Deploy to device"
if $BUILD_ISOMP4; then
    echo "  3. Verify qtdemux on device: gst-inspect-1.0 qtdemux"
fi
if $BUILD_PLAYBACK; then
    echo "  4. Verify decodebin on device: gst-inspect-1.0 decodebin"
fi
echo ""
info "The extension will now support MP4/MOV files."
