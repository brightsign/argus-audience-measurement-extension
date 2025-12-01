# Rockchip NPU Gaze Detection Architecture

**High-performance edge AI architecture for real-time computer vision on Rockchip NPU platforms**

This repository contains the technical architecture and design for an NPU-accelerated gaze detection and multi-model inference system targeting Rockchip SoCs with integrated NPU capabilities.

## Supported Hardware Platforms

The architecture supports three Rockchip NPU-enabled SoCs:

- **RK3588** (3-core NPU, 6 TOPS) - Supports 3 models in parallel
- **RK3576** (2-core NPU, 4 TOPS) - Supports 2 models in parallel
- **RK3568** (1-core NPU, 1 TOPS) - Supports 1 model

*Note: Documentation primarily focuses on the RK3588 implementation as the reference platform. Lower-tier SoCs use the same software architecture but run fewer models concurrently based on available NPU cores.*

## Architecture Highlights

**Software Design:**

- **C++ implementation** with optimized RKNN runtime for maximum efficiency
- **Multi-model parallelism** - run up to 3 AI models simultaneously on separate NPU cores
- **Zero-copy data pipelines** for minimal memory bandwidth usage
- **Sub-15ms inference latency** per model at full resolution
- **Modular pipeline architecture** supporting multiple input sources (RTSP, USB, RGBD cameras)

**Performance Characteristics (RK3588):**

- **0.80W incremental power per NPU core** for face detection workload
- **~5.5W total power** for 3-model parallel execution
- **3.3x more power-efficient** than Python implementations on identical hardware
- **Memory-efficient design** enables true 3-model parallel execution

## Supported Models

The architecture supports concurrent execution of multiple specialized models:

- **Face Detection** - RetinaFace for face localization and gaze estimation
- **Pose Estimation** - YOLOv8-pose for 17-keypoint skeleton tracking
- **Object Detection** - YOLOx for general object detection

Models are assigned to dedicated NPU cores with independent preprocessing pipelines and merged in post-processing for comprehensive scene understanding.

## Documentation

### Core Architecture

- **[Design Overview](docs/design.md)** - System architecture, pipeline design, and implementation details
- **[Multi-Model Architecture](docs/multiple-models.md)** - Technical design for parallel NPU execution across multiple cores

### Integration Guides

- **[RGB-D Camera Support](docs/rgbd.md)** - Integrating depth cameras for enhanced scene understanding

### Product Implementation

- **[BrightShopper](brightshopper/)** - Reference implementation for retail analytics use case

## Key Technical Features

**Modular Pipeline Design:**

- Pluggable input sources (RTSP, USB camera, RGBD)
- Configurable preprocessing per model type
- Independent NPU core affinity management
- Unified post-processing with multi-model fusion

**Optimized for Edge Deployment:**

- Low-latency real-time inference
- Minimal memory footprint
- Fanless operation on passively cooled hardware
- Production-ready error handling and recovery

**Multi-Model Capabilities:**

- Concurrent model execution on separate NPU cores
- Frame synchronization across models
- Cross-model result correlation
- Scalable architecture supporting 1-3 models based on available cores

## Performance Metrics (RK3588)

| Metric | Value | Notes |
|--------|-------|-------|
| **Incremental power/core** | +0.80W | C++ implementation vs +2.69W for Python |
| **Power (3-core workload)** | ~5.5W | Enables fanless operation |
| **Inference latency** | <15ms | Per model at full resolution |
| **Memory efficiency** | High | Zero-copy pipelines, enables 3-model parallel execution |

## Getting Started

### Prerequisites

- **Build System**: Yocto-based BrightSign OE build environment
- **Toolchain**: ARM cross-compilation toolchain for Rockchip SoCs
- **Dependencies**: RKNN SDK, GStreamer, MQTT client libraries
- **Target Device**: BrightSign player with RK3568, RK3576, or RK3588 SoC

### Build Instructions

**Build for All Platforms (LS5, XT5, Firebird)**:
```bash
# Full build - creates packages for all supported devices
./scripts/runall.sh --auto

# Output packages:
# - argus-ext-<timestamp>.zip (production package)
# - argus-dev-<timestamp>.zip (development package with debug symbols)
```

**Build for Single Platform (Faster)**:
```bash
# Build only for RK3568 (LS5) - ~13 seconds
./build-apps LS5

# Build only for RK3588 (XT5)
./build-apps XT5

# Build only for RK3576 (Firebird)
./build-apps Firebird

# Binaries output to: install/<platform>/attention_demo
```

**Clean Build**:
```bash
# Clean all build artifacts
./scripts/clean_build.sh

# Then rebuild
./scripts/runall.sh --auto
```

### Deployment

**Deploy to BrightSign Device**:
```bash
# 1. Copy package to device
scp argus-ext-<timestamp>.zip brightsign@<DEVICE_IP>:/storage/sd/

# 2. On device, extract and install
ssh brightsign@<DEVICE_IP>
cd /storage/sd
/var/volatile/bsext/ext_npu_argus/bsext_init stop
unzip argus-ext-<timestamp>.zip
bash ./ext_npu_argus_install-lvm.sh

# 3. Start the service
cd /var/volatile/bsext/ext_npu_argus
./bsext_init start

# 4. Check status
./bsext_init status

# 5. View logs
tail -f /storage/sd/logs/gaze.log
```

## Configuration

### Configuration File Priority

The application searches for configuration in this priority order:

1. **CLI argument**: `--config /path/to/config.json` (highest priority)
2. **Environment variable**: `export BSEXT_CONFIG=/path/to/config.json`
3. **SD card override**: `/storage/sd/configs/config.json` (writable, recommended for customization)
4. **Package default**: `/var/volatile/bsext/ext_npu_argus/RK3568/configs/config.json` (read-only)

### Customizing Configuration

**Recommended: Use SD Card Override** (persists across upgrades):
```bash
# 1. Create config directory on SD card
mkdir -p /storage/sd/configs

# 2. Copy default config as starting point
cp /var/volatile/bsext/ext_npu_argus/RK3568/configs/config.json /storage/sd/configs/

# 3. Edit your custom config
vi /storage/sd/configs/config.json

# 4. Restart service to apply changes
cd /var/volatile/bsext/ext_npu_argus
./bsext_init restart
```

### Key Configuration Options

**Log Level** - Control verbosity without recompiling:
```json
{
  "log_level": "info"
}
```
Options:
- `"debug"` - Most verbose (all logs including DEBUG messages)
- `"info"` - General information (recommended default)
- `"warn"` - Warnings and errors only (production mode)
- `"error"` - Errors only (minimal logging)

Change log level and restart service - no rebuild required!

**Input Source** - Switch between camera types:
```json
{
  "input_source": "rtsp",
  "input": {
    "rtsp_url": "rtsp://192.168.0.203:8554/live",
    "usb_device": "/dev/video0",
    "file_path": "/storage/sd/video.mp4"
  }
}
```
Options:
- `"rtsp"` - Network camera stream
- `"usb"` - USB webcam
- `"file"` - Video file for testing

Configure all three sources, then switch by changing `input_source` value.

**Model Configuration**:
```json
{
  "primary_model": {
    "name": "retinaface",
    "model_path": "model/retinaface.rknn",
    "npu_core": 1,
    "conf_threshold": 0.5,
    "nms_threshold": 0.45
  },
  "secondary_models": [
    {
      "name": "yolox",
      "model_path": "model/yolox_s.rknn",
      "npu_core": 0,
      "conf_threshold": 0.5
    }
  ]
}
```

Adjust confidence thresholds to tune detection sensitivity.

**Test Modes** - Debug individual models:
```json
{
  "test_face_only": false,
  "test_yolo_only": false
}
```
- Set `test_face_only: true` to run ONLY RetinaFace (disable YOLOX)
- Set `test_yolo_only: true` to run ONLY YOLOX (disable RetinaFace)
- Both `false` runs all models in parallel (production mode)

**MQTT Publishing**:
```json
{
  "publishers": [
    {
      "kind": "mqtt",
      "mqtt": {
        "host": "localhost",
        "port": 1883,
        "topic": "bs/argus/analytics",
        "qos": 0,
        "retain": false,
        "period_ms": 1000
      }
    }
  ]
}
```

### Verify Configuration

**Check which config is being used**:
```bash
# View startup logs
tail -100 /tmp/ext-npu-argus.log | grep "Config path selected"

# Expected output if using SD card:
# [INF] Config path selected: /storage/sd/configs/config.json

# Expected output if using package default:
# [INF] Config path selected: /var/volatile/bsext/ext_npu_argus/RK3568/configs/config.json
```

### Common Configuration Scenarios

**Production Mode** (minimal logging, network camera):
```json
{
  "log_level": "warn",
  "input_source": "rtsp",
  "test_face_only": false,
  "test_yolo_only": false
}
```

**Debug Mode** (verbose logging, USB camera, face detection only):
```json
{
  "log_level": "debug",
  "input_source": "usb",
  "test_face_only": true,
  "test_yolo_only": false
}
```

**Testing Mode** (video file loop):
```json
{
  "log_level": "info",
  "input_source": "file",
  "input": {
    "file_path": "/storage/sd/test-video.mp4",
    "file": {
      "loop": true
    }
  }
}
```

## Troubleshooting

**Check Application Logs**:
```bash
# View real-time logs
tail -f /tmp/ext-npu-argus.log

# View recent errors
tail -100 /tmp/ext-npu-argus.log | grep -E "ERR|WARN"

# Check startup sequence
tail -100 /tmp/ext-npu-argus.log | grep -E "Config path|Log level|input_source"
```

**Service Management**:
```bash
cd /var/volatile/bsext/ext_npu_argus

# Check status
./bsext_init status

# Restart service
./bsext_init restart

# Stop service
./bsext_init stop

# Start service
./bsext_init start
```

**Config Not Being Read**:
```bash
# Verify file exists and is readable
ls -la /storage/sd/configs/config.json
cat /storage/sd/configs/config.json | head -20

# Check for JSON syntax errors (should see valid JSON structure)
cat /storage/sd/configs/config.json | python3 -m json.tool > /dev/null && echo "Valid JSON" || echo "Invalid JSON"

# Verify application can access the file
tail /tmp/ext-npu-argus.log | grep "pick_config_path"
```

## License

*License information to be determined.*

---

**Technical Focus**: Computer vision architecture for Rockchip NPU platforms
**Key Applications**: Gaze detection, pose estimation, real-time behavioral analytics
