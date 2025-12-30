# BrightSign NPU Gaze Detection Extension

**Real-time edge AI for person tracking and gaze detection on Rockchip NPU platforms**

This repository contains a high-performance C++ application that runs on BrightSign players with integrated NPU capabilities. It captures video from cameras or streams, runs multiple neural network models in parallel for person detection and face/gaze analysis, tracks individuals across frames, and publishes analytics via MQTT.

## Supported Hardware

| Platform | SoC | NPU | Parallel Models |
|----------|-----|-----|-----------------|
| **XT5** | RK3588 | 3-core, 6 TOPS | 2-3 models |
| **XS156** | RK3576 | 2-core, 4 TOPS | 2 models |
| **LS5/HS5** | RK3568 | 1-core, 1 TOPS | 1 model |

## Key Features

**Multi-Model Parallel Inference:**
- **RetinaFace**: Face detection with 5-point landmarks for gaze estimation
- **YOLOX**: Person/object detection for accurate tracking

**Advanced Tracking:**
- ByteTrack or legacy EMA-based multi-object tracking
- Stable IDs across frame gaps and occlusions
- 8-way direction detection with confidence scoring
- Dwell time and enter/exit event detection

**Per-Person Gaze Analytics:**
- Associates face detections with tracked persons via IoU matching
- Determines if each person is looking at the camera
- Accumulates per-track gaze time

**Production-Ready:**
- Sub-15ms inference latency per model
- ~3.5W power consumption (enables fanless operation)
- Automatic recovery from camera disconnections
- MQTT, UDP, and file output options

## Performance

| Metric | Value |
|--------|-------|
| Inference latency | <15ms per model |
| End-to-end latency | 35-55ms |
| Power per NPU core | +0.80W |
| Processing rate | 25-30 FPS |

## Quick Start

### Build for All Platforms

```bash
# Full build - creates packages for all supported devices
./scripts/runall.sh --auto

# Output:
# - argus-ext-<timestamp>.zip (production package)
# - argus-dev-<timestamp>.zip (development package)
```

### Build for Single Platform

```bash
# RK3568 (LS5) - fastest build
./build-apps LS5

# RK3588 (XT5)
./build-apps XT5

# RK3576 (Firebird)
./build-apps Firebird

# Binary location: install/<platform>/attention_demo
```

### Deploy to Device

```bash
# Copy package to device
scp argus-ext-<timestamp>.zip brightsign@<DEVICE_IP>:/storage/sd/

# On device: install and start
ssh brightsign@<DEVICE_IP>
cd /storage/sd
unzip argus-ext-<timestamp>.zip
bash ./ext_npu_argus_install-lvm.sh
cd /var/volatile/bsext/ext_npu_argus
./bsext_init start

# Check status and logs
./bsext_init status
tail -f /tmp/ext-npu-argus.log
```

## Configuration

### Configuration Priority

1. **CLI argument**: `--config /path/to/argus-config.json`
2. **Environment variable**: `BSEXT_CONFIG=/path/to/argus-config.json`
3. **SD card override**: `/storage/sd/configs/argus-config.json` (recommended)
4. **Package default**: Built-in configuration

### Customizing Settings

Create or edit `/storage/sd/configs/argus-config.json`:

```json
{
  "log_level": "info",
  "input_source": "rtsp",
  "input": {
    "rtsp_url": "rtsp://192.168.0.100:8554/live",
    "usb_device": "/dev/video0"
  },
  "primary_model": {
    "name": "retinaface",
    "model_path": "model/retinaface.rknn",
    "npu_core": 0,
    "conf_threshold": 0.5
  },
  "secondary_model": {
    "name": "yolox",
    "model_path": "model/yolox_s.rknn",
    "npu_core": 1,
    "conf_threshold": 0.5
  },
  "publishers": [
    {
      "kind": "mqtt",
      "mqtt": {
        "host": "localhost",
        "port": 1883,
        "topic": "bs/argus/analytics",
        "period_ms": 1000
      }
    }
  ]
}
```

### Common Configurations

**Production (RTSP camera, minimal logging):**
```json
{
  "log_level": "warn",
  "input_source": "rtsp",
  "test_face_only": false,
  "test_yolo_only": false
}
```

**Debug (USB camera, verbose logging, face detection only):**
```json
{
  "log_level": "debug",
  "input_source": "usb",
  "test_face_only": true,
  "test_yolo_only": false
}
```

**Testing (video file loop):**
```json
{
  "log_level": "info",
  "input_source": "file",
  "input": {
    "file_path": "/storage/sd/test-video.mp4",
    "file": { "loop": true }
  }
}
```

## MQTT Output

Analytics are published to `bs/argus/analytics` using schema version `analytics/v7.0`:

```json
{
  "schema": "analytics/v7.0",
  "ts": 164.68,
  "device": "XS-156",
  "stream": "rtsp://192.168.0.100:8554/live",
  "frame_w": 1280,
  "frame_h": 720,
  "npu_load": 45.2,
  "people": 3,
  "gaze": 1,
  "fps": 29,
  "tracks": [
    {
      "id": 42,
      "bbox": [100, 200, 300, 600],
      "score": 0.93,
      "dir": "L",
      "speed": 55.3,
      "dwell": 12.5,
      "gaze": {
        "detected": 1,
        "time": 8.0,
        "face_bbox": [150, 180, 200, 240]
      }
    }
  ]
}
```

For complete schema documentation, see [MQTT Message Format](docs/mqtt-message-format.md).

## Architecture

The system uses a multi-threaded architecture with parallel model execution:

```
┌─────────────┐
│ Input Source│  RTSP / USB / File
└──────┬──────┘
       ▼
┌─────────────┐
│   Capture   │  Frame fetch, NV12→BGR, letterbox
│   Thread    │
└──────┬──────┘
       │ FrameMailbox (lock-free)
       ├─────────────────┐
       ▼                 ▼
┌─────────────┐   ┌─────────────┐
│  RetinaFace │   │    YOLOX    │
│  NPU Core 0 │   │  NPU Core 1 │
└──────┬──────┘   └──────┬──────┘
       │                 │
       ▼                 ▼
┌────────────────────────────────┐
│         Fusion State           │
│   (face_dets, yolo_dets, ...)  │
└────────────────┬───────────────┘
                 ▼
┌─────────────────────────────────┐
│ Supervisor: Tracker + Publisher │
│   - Person tracking (ByteTrack) │
│   - Gaze association            │
│   - MQTT publishing             │
└─────────────────────────────────┘
```

For detailed architecture documentation, see [Design Document](docs/DESIGN.md).

## Documentation

| Document | Description |
|----------|-------------|
| [Design Document](docs/DESIGN.md) | Full architecture, pipelines, and data structures |
| [MQTT Message Format](docs/mqtt-message-format.md) | Complete v7.0 schema reference with examples |
| [Multi-Model Architecture](docs/multiple-models.md) | Parallel NPU execution design |
| [RGB-D Camera Support](docs/rgbd.md) | Depth camera integration guide |

## Troubleshooting

### Check Logs

```bash
# Real-time logs
tail -f /tmp/ext-npu-argus.log

# Recent errors
tail -100 /tmp/ext-npu-argus.log | grep -E "ERR|WARN"

# Configuration loaded
tail -100 /tmp/ext-npu-argus.log | grep "Config path"
```

### Service Management

```bash
cd /var/volatile/bsext/ext_npu_argus
./bsext_init status   # Check status
./bsext_init restart  # Restart service
./bsext_init stop     # Stop service
```

### Common Issues

**No MQTT messages:**
- Check MQTT broker is running: `ps aux | grep mosquitto`
- Test subscription: `mosquitto_sub -h localhost -t 'bs/argus/#' -v`

**Camera not detected:**
- List USB devices: `ls -la /dev/video*`
- Check V4L2: `v4l2-ctl --list-devices`

**High NPU load / low FPS:**
- Reduce resolution in RTSP camera settings
- Enable only one model: `test_face_only: true` or `test_yolo_only: true`

## Uninstall

### Manual Removal

Connect to the player over SSH and drop to the Linux shell.

**Stop the extension:**

```bash
/var/volatile/bsext/ext_npu_argus/bsext_init stop
```

**Verify all processes for the extension have stopped:**

```bash
ps | grep -E "attention_demo|argus-exporter|mosquitto"
```

**Run the uninstall script:**

```bash
/var/volatile/bsext/ext_npu_argus/uninstall.sh
```

**Reboot to apply changes:**

```bash
reboot
```

## Build Requirements

- **Build Environment**: Yocto-based BrightSign OE build environment
- **Toolchain**: ARM cross-compilation toolchain for aarch64
- **C++ Standard**: C++20

### Dependencies

- RKNN SDK and runtime
- OpenCV 4.x
- GStreamer 1.0
- Mosquitto MQTT
- Boost (filesystem, system)

## License

*License information to be determined.*

## Support

- **Issues**: [GitHub Issues](https://github.com/BrightSign-Playground/brightsign-npu-gaze-extension-ng/issues)
- **Documentation**: See `/docs` folder
