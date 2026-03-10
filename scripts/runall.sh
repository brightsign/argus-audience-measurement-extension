#!/bin/bash

# BrightSign NPU Argus Extension - Complete Build Script
# This script automates all the steps from the README.md

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Global variables
AUTO_MODE=false
SKIP_ARCH_CHECK=false
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Function to print colored output
print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_header() {
    echo -e "\n${BLUE}=== $1 ===${NC}\n"
}

# Function to prompt user for continuation
prompt_continue() {
    if [ "$AUTO_MODE" = true ]; then
        print_status "Auto mode: Continuing automatically..."
        return 0
    fi

    local message="$1"
    echo -e "\n${YELLOW}NEXT STEPS:${NC}"
    echo "$message"
    echo
    read -p "Do you want to continue? (y/N): " -r
    echo
    if [[ ! $REPLY =~ ^[Yy]([Ee][Ss])?$ ]]; then
        print_status "Exiting..."
        exit 0
    fi
}

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Container runtime (docker or podman)
CONTAINER_CMD=""

# Function to detect container runtime (docker or podman)
detect_container_runtime() {
    if command_exists docker && docker info >/dev/null 2>&1; then
        CONTAINER_CMD="docker"
        print_status "Using Docker as container runtime"
    elif command_exists podman && podman info >/dev/null 2>&1; then
        CONTAINER_CMD="podman"
        print_status "Using Podman as container runtime"
    elif command_exists docker; then
        print_error "Docker is installed but not running. Please start Docker and try again."
        exit 1
    elif command_exists podman; then
        print_error "Podman is installed but not running. Please check podman and try again."
        exit 1
    else
        print_error "No container runtime found. Please install Docker or Podman:"
        print_error "  Docker: https://docs.docker.com/engine/install/"
        print_error "  Podman: https://podman.io/getting-started/installation"
        exit 1
    fi
}

# Function to check if container runtime is available (legacy compatibility)
check_docker_running() {
    detect_container_runtime
}

# Function to cleanup all generated files and directories
cleanup_all() {
    print_header "CLEANUP: Removing all generated files and directories"
    
    print_warning "This will remove ALL generated files including:"
    print_warning "- Downloaded BrightSign OS source files"
    print_warning "- Extracted directories (brightsign-oe)"
    print_warning "- Docker images (bsoe-build, rknn_tk2)"
    print_warning "- Build directories (build_xt5, build_rk3576, build_ls5)"
    print_warning "- SDK installation (sdk directory)"
    print_warning "- Toolkit repositories (toolkit directory)"
    print_warning "- Generated packages (*.zip files)"
    print_warning "- Install directory contents"
    
    if [ "$AUTO_MODE" != true ]; then
        echo
        read -p "Are you sure you want to proceed with cleanup? This cannot be undone! (y/N): " -r
        echo
        if [[ ! $REPLY =~ ^[Yy]([Ee][Ss])?$ ]]; then
            print_status "Cleanup cancelled."
            return 0
        fi
    fi
    
    cd "$PROJECT_ROOT"
    
    # Remove downloaded source files
    print_status "Removing downloaded source files..."
    rm -f brightsign-*.tar.gz
    rm -f brightsign-x86_64-cobra-toolchain-*.sh
    rm -f Dockerfile
    
    # Remove extracted directories
    print_status "Removing extracted directories..."
    if [ -d "brightsign-oe" ]; then
        # First try to make files writable and remove build artifacts
        if [ -d "brightsign-oe/build" ]; then
            print_status "Cleaning build artifacts..."
            if ! chmod -R u+w brightsign-oe/build 2>/dev/null; then
                print_warning "Could not make build files writable - some files may be owned by root (from Docker)"
            fi
            if ! rm -rf brightsign-oe/build 2>/dev/null; then
                print_warning "Could not remove brightsign-oe/build directory - trying with sudo..."
                if command_exists sudo; then
                    sudo rm -rf brightsign-oe/build 2>/dev/null || print_warning "Failed to remove build directory even with sudo"
                else
                    print_warning "No sudo available - build directory may remain"
                fi
            fi
        fi
        # Remove the entire directory with better error reporting
        if ! chmod -R u+w brightsign-oe 2>/dev/null; then
            print_warning "Could not make all brightsign-oe files writable"
        fi
        
        if ! rm -rf brightsign-oe 2>/dev/null; then
            print_warning "Could not remove brightsign-oe directory completely"
            print_warning "This is often due to Docker-created files with root ownership"
            print_warning "You may need to run: sudo rm -rf brightsign-oe"
            
            # Try to remove what we can and report what's left
            remaining_files=$(find brightsign-oe -type f 2>/dev/null | wc -l)
            remaining_dirs=$(find brightsign-oe -type d 2>/dev/null | wc -l)
            if [ "$remaining_files" -gt 0 ] || [ "$remaining_dirs" -gt 1 ]; then
                print_warning "Remaining: $remaining_files files in $remaining_dirs directories"
            fi
        fi
    fi
    
    # Remove build directories
    print_status "Removing build directories..."
    rm -rf build_xt5
    rm -rf build_rk3576
    rm -rf build_ls5
    
    # Remove SDK installation
    print_status "Removing SDK installation..."
    rm -rf sdk
    
    # Remove toolkit repositories
    print_status "Removing toolkit repositories..."
    rm -rf toolkit
    
    # Remove generated packages
    print_status "Removing generated packages..."
    rm -f argus-dev-*.zip
    rm -f argus-ext-*.zip
    rm -f gaze-dev-*.zip  # Legacy cleanup
    rm -f gaze-ext-*.zip  # Legacy cleanup
    
    # Clean install directory (but keep the directory itself)
    print_status "Cleaning install directory..."
    if [ -d "install" ]; then
        rm -rf install/RK3568
        rm -rf install/RK3576
        rm -rf install/RK3588
        rm -f install/bsext_init
        rm -f install/uninstall.sh
    fi
    
    # Remove srv directory
    print_status "Removing srv directory..."
    rm -rf srv
    
    # Remove container images (docker or podman)
    print_status "Removing container images..."

    # Detect available container runtime for cleanup
    local cleanup_cmd=""
    if command_exists docker && docker info >/dev/null 2>&1; then
        cleanup_cmd="docker"
    elif command_exists podman && podman info >/dev/null 2>&1; then
        cleanup_cmd="podman"
    fi

    if [ -n "$cleanup_cmd" ]; then
        # Handle bsoe-build image and containers
        if $cleanup_cmd images | grep -q "bsoe-build"; then
            print_status "Removing bsoe-build image..."

            # Check for containers using this image
            containers=$($cleanup_cmd ps -a --filter ancestor=bsoe-build --format "{{.ID}}" 2>/dev/null)
            if [ -n "$containers" ]; then
                print_status "Found containers using bsoe-build image, removing them first..."
                echo "$containers" | while read -r container_id; do
                    if [ -n "$container_id" ]; then
                        print_status "Stopping container: $container_id"
                        $cleanup_cmd stop "$container_id" 2>/dev/null || print_warning "Failed to stop container $container_id"
                        print_status "Removing container: $container_id"
                        $cleanup_cmd rm "$container_id" 2>/dev/null || print_warning "Failed to remove container $container_id"
                    fi
                done
            fi

            # Now try to remove the image
            if ! $cleanup_cmd rmi bsoe-build 2>/dev/null; then
                print_warning "Failed to remove bsoe-build image after container cleanup"
                print_warning "Try manually: $cleanup_cmd images | grep bsoe-build"
            fi
        fi

        # Handle rknn_tk2 image and containers
        if $cleanup_cmd images | grep -q "rknn_tk2"; then
            print_status "Removing rknn_tk2 image..."

            # Check for containers using this image
            containers=$($cleanup_cmd ps -a --filter ancestor=rknn_tk2 --format "{{.ID}}" 2>/dev/null)
            if [ -n "$containers" ]; then
                print_status "Found containers using rknn_tk2 image, removing them first..."
                echo "$containers" | while read -r container_id; do
                    if [ -n "$container_id" ]; then
                        print_status "Stopping container: $container_id"
                        $cleanup_cmd stop "$container_id" 2>/dev/null || print_warning "Failed to stop container $container_id"
                        print_status "Removing container: $container_id"
                        $cleanup_cmd rm "$container_id" 2>/dev/null || print_warning "Failed to remove container $container_id"
                    fi
                done
            fi

            # Now try to remove the image
            if ! $cleanup_cmd rmi rknn_tk2 2>/dev/null; then
                print_warning "Failed to remove rknn_tk2 image after container cleanup"
                print_warning "Try manually: $cleanup_cmd images | grep rknn_tk2"
            fi
        fi
    else
        print_warning "No container runtime available - skipping image cleanup"
        print_warning "Install Docker or Podman to manage container images"
    fi
    
    print_status "Cleanup completed successfully!"
    print_status "The project directory has been reset to its initial state."
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -auto|--auto)
            AUTO_MODE=true
            shift
            ;;
        --skip-arch-check)
            SKIP_ARCH_CHECK=true
            shift
            ;;
        -c|--clean)
            cleanup_all
            exit 0
            ;;
        -h|--help)
            echo "Usage: $0 [-auto|--auto] [--skip-arch-check] [--clean]"
            echo "  -auto: Run all steps without prompting for confirmation"
            echo "  --skip-arch-check: Skip x86_64 architecture check (for testing)"
            echo "  --clean: Remove all generated files, directories, and Docker images"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use -h for help"
            exit 1
            ;;
    esac
done

# STEP 0: Setup
step0_setup() {
    print_header "STEP 0: Setup"
    print_status "DEBUG: Entering step0_setup"
    
    prompt_continue "This will:
- Check container runtime (Docker or Podman)
- Clone Rockchip repositories (rknn-toolkit2, rknn_model_zoo)
- Download and build BrightSign OS SDK
- Provide instructions for unsecuring the player"

    # Check container runtime (docker or podman)
    print_status "DEBUG: Detecting container runtime..."
    detect_container_runtime

    # Check other required tools
    if ! command_exists git; then
        print_error "Git is not installed. Please install git first."
        exit 1
    fi
    
    if ! command_exists cmake; then
        print_error "CMake is not installed. Please install cmake first."
        exit 1
    fi
    
    if ! command_exists wget; then
        print_error "wget is not installed. Please install wget first."
        exit 1
    fi

    print_status "All required tools are installed"

    # Set project root environment variable
    export project_root="$PROJECT_ROOT"
    print_status "Project root set to: $project_root"

    # Clone supporting repositories
    print_status "Cloning supporting Rockchip repositories..."
    cd "$project_root"
    mkdir -p toolkit && cd toolkit

    if [ ! -d "rknn-toolkit2" ]; then
        git clone https://github.com/airockchip/rknn-toolkit2.git --depth 1 --branch v2.3.0
    else
        print_status "rknn-toolkit2 already exists"
    fi

    if [ ! -d "rknn_model_zoo" ]; then
        git clone https://github.com/airockchip/rknn_model_zoo.git --depth 1 --branch v2.3.0
    else
        print_status "rknn_model_zoo already exists"
    fi

    cd "$project_root"

    # Install BSOS SDK
    print_status "Setting up BrightSign OS SDK..."
    
    # Set OS version variables
    export BRIGHTSIGN_OS_MAJOR_VERSION=9.0
    export BRIGHTSIGN_OS_MINOR_VERSION=189
    export BRIGHTSIGN_OS_VERSION=${BRIGHTSIGN_OS_MAJOR_VERSION}.${BRIGHTSIGN_OS_MINOR_VERSION}
    
    # Download BrightSign OS source if not already downloaded
    if [ ! -d "brightsign-oe" ]; then
        print_status "Downloading BrightSign OS source..."
        wget "https://brightsignbiz.s3.amazonaws.com/firmware/opensource/${BRIGHTSIGN_OS_MAJOR_VERSION}/${BRIGHTSIGN_OS_VERSION}/brightsign-${BRIGHTSIGN_OS_VERSION}-src-dl.tar.gz"
        wget "https://brightsignbiz.s3.amazonaws.com/firmware/opensource/${BRIGHTSIGN_OS_MAJOR_VERSION}/${BRIGHTSIGN_OS_VERSION}/brightsign-${BRIGHTSIGN_OS_VERSION}-src-oe.tar.gz"
        print_status "Extracting BrightSign OS source..."
        tar -xzf "brightsign-${BRIGHTSIGN_OS_VERSION}-src-dl.tar.gz"
        tar -xzf "brightsign-${BRIGHTSIGN_OS_VERSION}-src-oe.tar.gz"
        
        # Apply custom recipes
        rsync -av bsoe-recipes/ brightsign-oe/
        
        # Clean up
        rm "brightsign-${BRIGHTSIGN_OS_VERSION}-src-dl.tar.gz"
        rm "brightsign-${BRIGHTSIGN_OS_VERSION}-src-oe.tar.gz"
    else
        print_status "BrightSign OS source already downloaded"
    fi

    # Build SDK in container
    if [ ! -f "Dockerfile" ]; then
        print_status "Downloading Dockerfile..."
        wget https://raw.githubusercontent.com/brightsign/extension-template/refs/heads/main/Dockerfile
    fi

    if ! $CONTAINER_CMD images | grep -q "bsoe-build"; then
        print_status "Building BSOS container image..."
        $CONTAINER_CMD build --rm --build-arg USER_ID=$(id -u) --build-arg GROUP_ID=$(id -g) --ulimit memlock=-1:-1 -t bsoe-build .
    else
        print_status "BSOS container image already exists"
    fi

    mkdir -p srv

    # Check if SDK already exists
    if [ ! -f "brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh" ]; then
        print_status "Building BrightSign SDK (this may take several hours)..."
        $CONTAINER_CMD run -it --rm \
            -v $(pwd)/brightsign-oe:/home/builder/bsoe \
            -v $(pwd)/srv:/srv \
            bsoe-build \
            bash -c "cd /home/builder/bsoe/build && MACHINE=cobra ./bsbb brightsign-sdk"
        
        # Copy the SDK
        cp brightsign-oe/build/tmp-glibc/deploy/sdk/brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh ./
    else
        print_status "SDK already exists"
    fi

    # Install SDK
    if [ ! -d "sdk" ]; then
        print_status "Installing SDK..."
        ./brightsign-x86_64-cobra-toolchain-${BRIGHTSIGN_OS_VERSION}.sh -d ./sdk -y
        
        # Patch SDK with Rockchip libraries
        cd sdk/sysroots/aarch64-oe-linux/usr/lib
        if [ ! -f "librknnrt.so" ]; then
            wget https://github.com/airockchip/rknn-toolkit2/raw/v2.3.2/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so
        fi
        cd "$project_root"
    else
        print_status "SDK already installed"
    fi

    print_status "Step 0 completed successfully!"
    print_status "DEBUG: Exiting step0_setup successfully"
    
    print_warning "MANUAL STEP REQUIRED: You need to unsecure your BrightSign player"
    print_warning "Follow the instructions in the README.md under 'Unsecure the Player'"
    print_warning "This involves connecting serial cable and using boot commands"
}

# STEP 1: Compile ONNX Models
step1_compile_models() {
    print_header "STEP 1: Compile ONNX Models for Rockchip NPU"
    print_status "DEBUG: Entering step1_compile_models"
    
    prompt_continue "This will:
- Build Docker container for model compilation
- Download and compile RetinaFace models (all platforms)
- Download and compile YOLOX models (all platforms)"

    cd "$project_root"
    print_status "DEBUG: Changed to project root: $project_root"
    
    # Use the compile-models script for all model compilation
    if [ -f "./compile-models" ]; then
        chmod +x ./compile-models
        
        print_status "Compiling all AI models (RetinaFace + YOLOX)..."
        
        # Compile RetinaFace for all platforms
        print_status "Compiling RetinaFace models..."
        ./compile-models || print_warning "RetinaFace compilation may have skipped already compiled models"
        
        # Compile YOLOX for all platforms
        print_status "Compiling YOLOX models for object detection..."
        ./compile-models yolox || print_warning "YOLOX compilation may have skipped already compiled models"
    else
        print_warning "compile-models script not found"
        return 1
    fi

    print_status "Step 1 completed successfully!"
    print_status "DEBUG: Exiting step1_compile_models successfully"
}

# STEP 3: Build and Test on XT5
step3_build_xt5() {
    print_header "STEP 3: Build and Test"
    print_status "DEBUG: Entering step3_build_xt5"
    
    prompt_continue "This will:
- Build application for XT5 (RK3588)
- Build application for RK3576
- Build application for LS5 (RK3568)
- Install binaries and libraries to install directory"

    cd "$project_root"
    print_status "DEBUG: Changed to project root: $project_root"
    
    # Source the SDK environment
    print_status "DEBUG: Sourcing SDK environment..."
    source ./sdk/environment-setup-aarch64-oe-linux

    # Demo mode defaults to ON in CMakeLists.txt. Production builds must
    # explicitly set DEMO_MODE=0 to compile out expiration enforcement.
    local demo_cmake_flag=""
    if [[ "${DEMO_MODE:-1}" != "1" ]]; then
        demo_cmake_flag="-DENABLE_DEMO_MODE=OFF"
        print_status "Demo mode DISABLED: building production binary without expiration enforcement"
    else
        print_status "Demo mode ENABLED: building with expiration date enforcement"
    fi

    # Build for XT5 (RK3588)
    print_status "Building for XT5 (RK3588)..."
    rm -rf build_xt5
    mkdir -p build_xt5 && cd build_xt5

    cmake .. -DOECORE_TARGET_SYSROOT="${OECORE_TARGET_SYSROOT}" -DTARGET_SOC="rk3588" -DBUILD_TESTS=OFF ${demo_cmake_flag}
    make
    make install

    cd "$project_root"

    # Build for RK3576
    print_status "Building for RK3576..."
    rm -rf build_rk3576
    mkdir -p build_rk3576 && cd build_rk3576

    cmake .. -DOECORE_TARGET_SYSROOT="${OECORE_TARGET_SYSROOT}" -DTARGET_SOC="rk3576" -DBUILD_TESTS=OFF ${demo_cmake_flag}
    make
    make install

    cd "$project_root"

    # Build for LS5 (RK3568)
    print_status "Building for LS5 (RK3568)..."
    rm -rf build_ls5
    mkdir -p build_ls5 && cd build_ls5

    cmake .. -DOECORE_TARGET_SYSROOT="${OECORE_TARGET_SYSROOT}" -DTARGET_SOC="rk3568" -DBUILD_TESTS=OFF ${demo_cmake_flag}
    make
    make install

    cd "$project_root"

    print_status "Step 3 completed successfully!"
    print_status "DEBUG: Exiting step3_build_xt5 successfully"
}

# STEP 3b: Build GStreamer plugins for MP4 support (optional)
step3b_build_gstreamer_plugins() {
    print_header "STEP 3b: Build GStreamer Plugins (MP4 Support)"
    print_status "DEBUG: Entering step3b_build_gstreamer_plugins"

    prompt_continue "This will:
- Build libgstisomp4.so (qtdemux for MP4/MOV demuxing)
- Build libgstplayback.so (decodebin for auto-detection)
- Copy plugins to install directories
Note: This step is optional - only needed for MP4 file input support"

    cd "$project_root"
    print_status "DEBUG: Changed to project root: $project_root"

    if [ -f "./scripts/build-gst-isomp4-plugin.sh" ]; then
        chmod +x ./scripts/build-gst-isomp4-plugin.sh
        print_status "Building GStreamer plugins for MP4 support..."
        print_status "DEBUG: Executing ./scripts/build-gst-isomp4-plugin.sh"
        # Run with || true to prevent set -e from stopping on errors (this step is optional)
        ./scripts/build-gst-isomp4-plugin.sh || true
        local exit_code=$?
        print_status "DEBUG: build-gst-isomp4-plugin.sh exit code: $exit_code"
        if [ $exit_code -ne 0 ]; then
            print_warning "GStreamer plugin build returned non-zero exit code: $exit_code"
            print_warning "This is optional - continuing anyway"
        fi
    else
        print_warning "build-gst-isomp4-plugin.sh not found - skipping"
        print_warning "MP4 file input will not be supported"
    fi

    print_status "Step 3b completed!"
    print_status "DEBUG: Exiting step3b_build_gstreamer_plugins successfully"
}

# STEP 4: Package the Extension
step4_package() {
    print_header "STEP 4: Package the Extension"
    print_status "DEBUG: Entering step4_package"

    prompt_continue "This will:
- Use package script to create packages
- Create development package (argus-dev)
- Create production extension package (argus-ext)"

    cd "$project_root"
    print_status "DEBUG: Changed to project root: $project_root"
    
    # Use the package script which handles everything correctly
    if [ -f "./package" ]; then
        chmod +x ./package
        print_status "Running package script..."
        print_status "DEBUG: Executing ./package"
        ./package
        local exit_code=$?
        print_status "DEBUG: package script exit code: $exit_code"
        if [ $exit_code -ne 0 ]; then
            print_error "Package script failed with exit code: $exit_code"
            return 1
        fi
    else
        print_error "package script not found!"
        return 1
    fi
    
    print_status "Step 4 completed successfully!"
    print_status "DEBUG: Exiting step4_package successfully"
    print_status "Development package: argus-dev-*.zip"
    print_status "Production extension: argus-ext-*.zip"
    
    # List the created packages for verification
    print_status "DEBUG: Listing created packages:"
    ls -lh argus-*.zip 2>/dev/null || print_warning "No argus-*.zip files found!"
}

# Main execution
main() {
    print_header "BrightSign NPU Argus Extension - Complete Build"
    print_status "DEBUG: Script started with arguments: $@"
    
    if [ "$AUTO_MODE" = true ]; then
        print_status "Running in automatic mode - no prompts"
    else
        print_status "Running in interactive mode - will prompt between steps"
    fi
    
    print_status "Project root: $PROJECT_ROOT"
    print_status "DEBUG: Starting main build sequence"
    
    # Check architecture
    if [ "$(uname -m)" != "x86_64" ] && [ "$SKIP_ARCH_CHECK" != true ]; then
        print_error "This script requires x86_64 architecture"
        print_error "Current architecture: $(uname -m)"
        print_error "Use --skip-arch-check to bypass this check for testing"
        exit 1
    elif [ "$SKIP_ARCH_CHECK" = true ]; then
        print_warning "Skipping architecture check - this is for testing only"
    fi
    
    # Execute steps
    print_status "DEBUG: About to execute step0_setup"
    step0_setup
    print_status "DEBUG: step0_setup completed, moving to step1_compile_models"
    
    step1_compile_models
    print_status "DEBUG: step1_compile_models completed, moving to step3_build_xt5"
    
    step3_build_xt5
    print_status "DEBUG: step3_build_xt5 completed, moving to step3b_build_gstreamer_plugins"
    
    step3b_build_gstreamer_plugins
    print_status "DEBUG: step3b_build_gstreamer_plugins completed, moving to step4_package"
    
    step4_package
    print_status "DEBUG: step4_package completed"
    
    print_header "BUILD COMPLETE"
    print_status "All steps completed successfully!"
    print_status "Check the install directory for the built files"
    print_status "Development package: argus-dev-*.zip"
    print_status "Production extension: argus-ext-*.zip"
    
    # Final verification
    print_status "DEBUG: Final package verification:"
    ls -lh argus-*.zip 2>/dev/null || print_warning "WARNING: No argus-*.zip files found in project root!"
    
    print_warning "Don't forget to unsecure your BrightSign player as described in the README!"
}

# Run main function
main "$@"
