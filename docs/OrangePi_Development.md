# Orange Pi Development Guide - Argus Audience Measurement Extension

This guide covers development and testing using Orange Pi boards as an alternative development environment for the Argus Audience Measurement Extension project.

## Overview

Orange Pi boards with Rockchip SoCs (RK3588, RK3568, RK3576) provide an excellent native development environment that eliminates cross-compilation overhead. This approach enables:

- **Rapid iteration**: Native compilation is faster than cross-compiling
- **Full debugging**: GDB, valgrind, and other tools work natively
- **Real hardware testing**: Identical NPU hardware to BrightSign players
- **Easier troubleshooting**: Full Linux environment with standard tools

**Important Limitations**:
- Model compilation (RKNN toolkit) requires x86_64 - cannot run on Orange Pi
- Final deployment packages must still be cross-compiled for BrightSign OS

## Hardware Mapping

| Orange Pi Board | Rockchip SoC | BrightSign Equivalent | Build Target |
|----------------|--------------|----------------------|--------------|
| Orange Pi 5 Plus | RK3588 | XT5 | `XT5` / `rk3588` |
| Orange Pi 5 | RK3588 | XT5 | `XT5` / `rk3588` |
| Orange Pi 3B | RK3568 | LS5 | `LS5` / `rk3568` |
| Orange Pi CM5 | RK3576 | XS156/Firebird | `Firebird` / `rk3576` |

## Prerequisites

### Hardware Requirements
- Orange Pi 5/5 Plus (recommended for RK3588 development)
- USB webcam (Logitech C270 recommended)
- Network connection (for RTSP testing)
- USB-C power supply (5V/4A minimum)

### Software Dependencies

Install required packages on your Orange Pi (Debian/Ubuntu-based):

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    gdb \
    valgrind \
    pkg-config \
    libboost-all-dev \
    libopencv-dev \
    libturbojpeg0-dev \
    libssl-dev \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libglib2.0-dev \
    libmosquitto-dev \
    mosquitto \
    mosquitto-clients \
    v4l-utils \
    socat \
    htop \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-libav
```

### RGA Library (Rockchip Graphics Acceleration)

On Orange Pi 5/5 Plus with proper OS support, RGA should already be installed:

```bash
# Check if RGA is installed
dpkg -l | grep -i rga

# Should show librga2 and librga-dev
# If not installed, check your OS image or install from Rockchip sources
```

If RGA is missing on an RK3588 system, you can build from source:
```bash
git clone https://github.com/airockchip/librga.git
cd librga
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
sudo ldconfig
```

**Note**: The build system will continue without RGA if not found, but hardware-accelerated 2D operations will be unavailable.

## Setting Up the Development Environment

### 1. Clone the Repository

```bash
git clone <repository-url> argus-audience-measurement-extension
cd argus-audience-measurement-extension
```

### 2. Install RKNN Runtime Libraries

The RKNN runtime libraries are required for NPU inference. Download from the official Rockchip repository:

```bash
# Create lib directory for runtime libraries
mkdir -p lib

# Download RKNN runtime (adjust version as needed)
# For RK3588 (Orange Pi 5/5 Plus):
wget -O lib/librknnrt.so \
    "https://github.com/airockchip/rknn-toolkit2/raw/v2.3.0/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so"

# Also copy to include directory (required by CMakeLists.txt)
cp lib/librknnrt.so include/
```

Alternatively, if the libraries are already present in `/usr/lib`:
```bash
# Check if RKNN runtime is installed system-wide
ls -la /usr/lib/librknnrt.so*

# If present, create symlink
ln -sf /usr/lib/librknnrt.so include/librknnrt.so
```

### 3. Prepare Models

Models must be compiled on an x86_64 machine first. Copy pre-compiled models to your Orange Pi:

```bash
# From your x86_64 development machine:
scp -r install/RK3588/model/ user@orangepi:/path/to/argus-audience-measurement-extension/install/RK3588/

# Required models:
# - model/retinaface.rknn (face detection)
# - model/yolox_s.rknn (object detection, optional)
```

Or ensure models exist in the install directory:
```bash
ls -la install/RK3588/model/
# Should contain: retinaface.rknn (and optionally yolox_s.rknn)
```

## Building on Orange Pi

### Native Build Process

The CMakeLists.txt automatically detects ARM64 architecture and configures for native build:

```bash
# Create build directory
mkdir -p build_opi
cd build_opi

# Configure with CMake
cmake .. -DTARGET_SOC=rk3588 -DCMAKE_BUILD_TYPE=Debug

# Build
make -j$(nproc)
```

### Build Output

After successful build, you'll have:
```
build_opi/
├── attention_demo          # Main executable
├── librknnrt.so           # RKNN runtime (copied)
├── librga.so              # RGA library (if available)
└── model/                 # Model files (copied)
```

### Build Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `-DTARGET_SOC=rk3588` | Target SoC (rk3588, rk3568, rk3576) | rk3588 |
| `-DCMAKE_BUILD_TYPE=Debug` | Build type (Debug, Release) | Debug |
| `-DCMAKE_VERBOSE_MAKEFILE=ON` | Verbose make output | OFF |

## Running and Testing

### Basic Execution

```bash
cd build_opi

# Set library path
export LD_LIBRARY_PATH=.:./lib:$LD_LIBRARY_PATH

# Symlink model directory if not present
ln -sf ../install/RK3588/model ./model

# Run with default config
./attention_demo

# Run with custom config
./attention_demo --config ../configs/config.json
```

### Stopping the Application

**Note**: The application may not respond to Ctrl-C due to custom signal handling and threaded cleanup. If Ctrl-C doesn't work:

```bash
# Kill by process name
pkill -9 attention_demo

# Or find PID and force kill
ps aux | grep attention_demo
kill -9 <pid>
```

### Configuration File

The application uses `configs/config.json` for all settings. Key configurations for Orange Pi testing:

```json
{
  "input_source": "usb",
  "input": {
    "usb_device": "/dev/video0",
    "usb": {
      "width": 640,
      "height": 480,
      "fps": 30
    }
  },
  "primary_model": {
    "name": "retinaface",
    "model_path": "model/retinaface.rknn"
  },
  "log_level": "debug",
  "enable_frame_output": true,
  "output_dir": "/tmp"
}
```

### Input Source Options

#### USB Camera
```json
{
  "input_source": "usb",
  "input": {
    "usb_device": "/dev/video0"
  }
}
```

Find your camera device:
```bash
v4l2-ctl --list-devices
# Usually /dev/video0 on Orange Pi
```

#### RTSP Stream
```json
{
  "input_source": "rtsp",
  "input": {
    "rtsp_url": "rtsp://192.168.1.100:8554/live"
  }
}
```

#### Video File
```json
{
  "input_source": "file",
  "input": {
    "file_path": "/path/to/test_video.mp4",
    "file": {
      "loop": true
    }
  }
}
```

### Testing with MQTT

The application includes an embedded MQTT broker. Start it separately for testing:

```bash
# Terminal 1: Start MQTT broker
mosquitto -v

# Terminal 2: Subscribe to analytics topic
mosquitto_sub -h localhost -t "bs/argus/analytics" -v

# Terminal 3: Run the application
cd build_opi
./attention_demo
```

Expected MQTT output format:
```json
{
  "device_id": "auto-detected-mac",
  "faces_total": 2,
  "faces_attending": 1,
  "persons_total": 3,
  "timestamp": 1735344000
}
```

### Viewing Annotated Output

When `enable_frame_output` is true, annotated frames are written to the output directory:

```bash
# Watch for output images
watch -n 0.5 "ls -la /tmp/*.jpg"

# View images (if X11 available)
feh /tmp/output.jpg

# Or copy to your dev machine for viewing
scp orangepi:/tmp/output.jpg ./
```

## Debugging

### GDB Debugging

```bash
cd build_opi

# Start GDB
gdb ./attention_demo

# Set breakpoints
(gdb) break main
(gdb) break src/attention.cpp:100

# Run with arguments
(gdb) run --config ../configs/config.json

# Useful GDB commands
(gdb) bt        # Backtrace
(gdb) info threads
(gdb) thread 2  # Switch to thread
(gdb) p variable_name
```

### Memory Debugging with Valgrind

```bash
# Check for memory leaks
valgrind --leak-check=full ./attention_demo --config ../configs/config.json

# Check for threading issues
valgrind --tool=helgrind ./attention_demo --config ../configs/config.json
```

### Logging

Configure log level in config.json:
- `debug`: All messages including debug output
- `info`: Informational messages and above
- `warn`: Warnings and errors only
- `error`: Errors only

```bash
# View logs (if file logging enabled)
tail -f /tmp/logs/attention_demo.log

# Or monitor stdout
./attention_demo 2>&1 | tee debug.log
```

### Performance Monitoring

```bash
# Monitor CPU/memory usage
htop

# NPU utilization (RK3588)
cat /sys/kernel/debug/rknpu/load
# Or
watch -n 1 cat /sys/kernel/debug/rknpu/load

# Camera capabilities
v4l2-ctl --device=/dev/video0 --all
```

## Development Workflow

### Recommended Workflow

1. **Develop on Orange Pi**: Edit code, build natively, test immediately
2. **Debug issues**: Use GDB, logs, and valgrind
3. **Iterate quickly**: Make changes and rebuild in seconds
4. **Once stable**: Cross-compile for BrightSign deployment

### Syncing Code

Use rsync for efficient code synchronization:

```bash
# From your main development machine to Orange Pi:
rsync -avz --exclude='build*' --exclude='sdk' --exclude='.git' \
    /path/to/argus-audience-measurement-extension/ \
    user@orangepi:/home/user/argus-audience-measurement-extension/

# From Orange Pi back to dev machine:
rsync -avz --exclude='build*' \
    user@orangepi:/home/user/argus-audience-measurement-extension/src/ \
    /path/to/argus-audience-measurement-extension/src/
```

### VS Code Remote Development

For the best experience, use VS Code with Remote-SSH:

1. Install "Remote - SSH" extension
2. Connect to Orange Pi: `Ctrl+Shift+P` -> "Remote-SSH: Connect to Host"
3. Open the project folder
4. Install C/C++ extension on remote
5. Configure `.vscode/c_cpp_properties.json` for IntelliSense

## Troubleshooting

### Common Issues

#### "librknnrt.so: cannot open shared object file"
```bash
export LD_LIBRARY_PATH=.:./lib:/usr/lib:$LD_LIBRARY_PATH
# Or copy to system lib directory
sudo cp librknnrt.so /usr/lib/
sudo ldconfig
```

#### Camera not detected
```bash
# Check camera is connected
lsusb | grep -i cam

# Check video devices
ls -la /dev/video*

# Test camera directly
v4l2-ctl --device=/dev/video0 --stream-mmap --stream-count=1 --stream-to=test.raw
```

#### RKNN model loading fails
```bash
# Verify model file exists and is correct format
ls -la model/retinaface.rknn

# Check model was compiled for correct SoC
# Model must match: RK3588 model for OPi5, RK3568 for OPi3B, etc.
```

#### NPU not available
```bash
# Check NPU device exists
ls -la /dev/dri/
ls -la /sys/class/devfreq/*npu*

# Verify kernel module loaded
lsmod | grep rknpu
```

### Performance Tuning

For optimal performance on Orange Pi:

```bash
# Set CPU governor to performance
sudo cpufreq-set -g performance

# Set NPU to max frequency (RK3588)
echo performance | sudo tee /sys/class/devfreq/fdab0000.npu/governor
```

## Cross-Compilation Workflow

When ready to deploy to BrightSign, use the cross-compilation build system on your x86_64 machine:

```bash
# On x86_64 development machine

# Build for specific platform
./build-apps XT5      # For RK3588/XT5
./build-apps LS5      # For RK3568/LS5
./build-apps Firebird # For RK3576/XS156

# Build for all platforms
./build-apps

# Output in install/ directory
ls install/RK3588/
# attention_demo, lib/, bin/, model/, configs/
```

## Quick Reference

### Essential Commands

```bash
# Build
mkdir -p build_opi && cd build_opi
cmake .. -DTARGET_SOC=rk3588 && make -j$(nproc)

# Run
export LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH
./attention_demo --config ../configs/config.json

# Test MQTT output
mosquitto_sub -h localhost -t "bs/argus/analytics" -v

# Debug
gdb ./attention_demo

# Monitor performance
htop & watch -n 1 cat /sys/kernel/debug/rknpu/load
```

### Directory Structure

```
argus-audience-measurement-extension/
├── build_opi/           # Native Orange Pi build (create this)
├── configs/
│   └── config.json      # Runtime configuration
├── include/
│   └── librknnrt.so     # RKNN runtime library
├── install/
│   └── RK3588/
│       ├── model/       # Compiled RKNN models
│       └── lib/         # Runtime libraries
├── src/                 # Source code
└── docs/                # Documentation
```

### Environment Variables

```bash
# Required
export LD_LIBRARY_PATH=.:./lib:$LD_LIBRARY_PATH

# Optional - for GStreamer debugging
export GST_DEBUG=2
export GST_PLUGIN_PATH=/usr/lib/gstreamer-1.0
```

## See Also

- [DESIGN.md](DESIGN.md) - Architecture and design documentation
- [mqtt-message-format.md](mqtt-message-format.md) - MQTT message specifications
- [multiple-models.md](multiple-models.md) - Multi-model inference guide
