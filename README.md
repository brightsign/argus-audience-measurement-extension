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

*Documentation for building and deploying the gaze detection system coming soon.*

## License

*License information to be determined.*

---

**Technical Focus**: Computer vision architecture for Rockchip NPU platforms
**Key Applications**: Gaze detection, pose estimation, real-time behavioral analytics
