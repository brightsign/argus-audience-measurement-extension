#!/bin/bash
# Install missing GStreamer plugins for file input support
# This script helps locate and copy libgstisomp4.so and libgstplayback.so

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info() { echo -e "${BLUE}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[✓]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }

# Required plugins
REQUIRED_PLUGINS=(
    "libgstisomp4.so"     # Provides qtdemux (MP4/MOV demuxer)
    "libgstplayback.so"   # Provides decodebin/uridecodebin
)

# Search paths (adjust based on your SDK location)
SEARCH_PATHS=(
    "$PROJECT_ROOT/sdk/sysroots/*/usr/lib/gstreamer-1.0"
    "$PROJECT_ROOT/brightsign-oe/build/tmp/sysroots/*/usr/lib/gstreamer-1.0"
    "$PROJECT_ROOT/brightsign-oe/build/tmp/work/*/gst-plugins-*/image/usr/lib/gstreamer-1.0"
    "/usr/lib/aarch64-linux-gnu/gstreamer-1.0"
    "/usr/lib/gstreamer-1.0"
)

# Destination for each platform
PLATFORMS=("RK3568" "RK3588" "RK3576")

usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Locate and copy missing GStreamer plugins needed for file input.

OPTIONS:
    --search-only    Only search for plugins, don't copy
    --platform NAME  Only install for specific platform (RK3568, RK3588, RK3576)
    --source DIR     Use specific directory as plugin source
    --help           Show this help message

EXAMPLES:
    # Search for plugins and copy to all platforms
    $0

    # Only search without copying
    $0 --search-only

    # Install only for LS5 (RK3568)
    $0 --platform RK3568

    # Use custom source directory
    $0 --source /path/to/gstreamer-plugins

REQUIRED PLUGINS:
    - libgstisomp4.so   : Provides qtdemux (MP4/MOV demuxer)
    - libgstplayback.so : Provides decodebin/uridecodebin for auto-detection

EOF
}

find_plugin() {
    local plugin_name="$1"
    
    # Try user-provided source first
    if [[ -n "$SOURCE_DIR" ]]; then
        local found="$(find "$SOURCE_DIR" -name "$plugin_name" -type f 2>/dev/null | head -1)"
        if [[ -n "$found" ]]; then
            echo "$found"
            return 0
        fi
    fi
    
    # Search in known paths
    for search_path in "${SEARCH_PATHS[@]}"; do
        local found="$(find $search_path -name "$plugin_name" -type f 2>/dev/null | head -1)"
        if [[ -n "$found" ]]; then
            echo "$found"
            return 0
        fi
    done
    
    return 1
}

verify_plugin() {
    local plugin_path="$1"
    
    # Check it's a valid ELF file
    if ! file "$plugin_path" | grep -q "ELF.*LSB.*ARM aarch64"; then
        warn "$plugin_path is not an aarch64 library"
        return 1
    fi
    
    # Check dependencies are reasonable
    if ! ldd "$plugin_path" >/dev/null 2>&1; then
        warn "$plugin_path has missing dependencies (but may work on target device)"
    fi
    
    return 0
}

# Parse arguments
SEARCH_ONLY=false
SOURCE_DIR=""
TARGET_PLATFORM=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --search-only)
            SEARCH_ONLY=true
            shift
            ;;
        --platform)
            TARGET_PLATFORM="$2"
            shift 2
            ;;
        --source)
            SOURCE_DIR="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

info "Searching for required GStreamer plugins..."
echo

# Find plugins
declare -A FOUND_PLUGINS

for plugin in "${REQUIRED_PLUGINS[@]}"; do
    info "Searching for $plugin..."
    
    if plugin_path=$(find_plugin "$plugin"); then
        success "Found: $plugin_path"
        FOUND_PLUGINS[$plugin]="$plugin_path"
        
        # Verify it
        if verify_plugin "$plugin_path"; then
            success "Verified: $plugin is a valid aarch64 library"
        fi
    else
        error "Not found: $plugin"
    fi
    echo
done

# Check if all plugins found
MISSING_COUNT=0
for plugin in "${REQUIRED_PLUGINS[@]}"; do
    if [[ -z "${FOUND_PLUGINS[$plugin]}" ]]; then
        ((MISSING_COUNT++))
    fi
done

if [[ $MISSING_COUNT -gt 0 ]]; then
    error "$MISSING_COUNT plugin(s) not found!"
    echo
    warn "Please ensure you have built the GStreamer plugins or provide --source path"
    warn "You may need to:"
    warn "  1. Build gst-plugins-good (contains libgstisomp4.so)"
    warn "  2. Build gst-plugins-base (contains libgstplayback.so)"
    warn "  3. Or copy from a working BrightSign device"
    exit 1
fi

success "All required plugins found!"
echo

# If search-only mode, exit here
if $SEARCH_ONLY; then
    info "Search-only mode. Exiting without copying."
    exit 0
fi

# Copy plugins to install directories
info "Copying plugins to install directories..."
echo

PLATFORMS_TO_INSTALL=("${PLATFORMS[@]}")
if [[ -n "$TARGET_PLATFORM" ]]; then
    PLATFORMS_TO_INSTALL=("$TARGET_PLATFORM")
fi

for platform in "${PLATFORMS_TO_INSTALL[@]}"; do
    DEST_DIR="$PROJECT_ROOT/install/$platform/lib/gstreamer-1.0"
    
    info "Installing for $platform..."
    
    # Create directory if needed
    mkdir -p "$DEST_DIR"
    
    for plugin in "${REQUIRED_PLUGINS[@]}"; do
        src="${FOUND_PLUGINS[$plugin]}"
        dest="$DEST_DIR/$plugin"
        
        if [[ -f "$dest" ]]; then
            warn "$plugin already exists in $platform, overwriting..."
        fi
        
        cp -v "$src" "$dest"
        chmod 644 "$dest"
        
        success "Copied $plugin to $platform"
    done
    
    echo
done

success "Plugin installation complete!"
echo

info "Next steps:"
echo "  1. Run: make clean && make"
echo "  2. Run: bash ./package --ext-only"
echo "  3. Deploy package to device"
echo "  4. Verify on device:"
echo "     gst-inspect-1.0 qtdemux"
echo "     gst-inspect-1.0 decodebin"
echo

info "The plugins will be automatically included in the next package build."
