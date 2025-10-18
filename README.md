# BrightSign NPU Platform

**Next-generation edge AI architecture for BrightSign digital signage players**

This repository contains the NPU-accelerated software platform powering BrightSign's latest products based on the Rockchip RK3588 processor. The platform leverages the RK3588's integrated 3-core NPU (6 TOPS) to deliver real-time computer vision and AI analytics at the edgewith no cloud dependency, zero latency, and complete data privacy.

## Platform Highlights

**Hardware Foundation:**

The platform supports three Rockchip NPU-enabled SoCs, each targeting different performance and cost requirements:

- **RK3588** (3-core NPU, 6 TOPS) - Premium tier, used in **XT5**
- **RK3576** (2-core NPU, 4 TOPS) - Mid-tier, used in **XS156**
- **RK3568** (1-core NPU, 1 TOPS) - Entry tier, used in **LS5/HS5**

**Performance characteristics (using XT5/RK3588 as reference):**
- **5.5W total power** for 3-model AI workload
- **Fanless operation** with passive cooling
- **11-year expected lifespan** (100,000-hour MTBF)

*Note: This documentation primarily focuses on the XT5/RK3588 implementation. Lower-tier products (XS156, LS5/HS5) support fewer simultaneous models but use the same software architecture and C++ implementation.*

**Software Architecture:**
- **C++ implementation** with optimized RKNN runtime for maximum efficiency
- **Multi-model parallelism** - run up to 3 AI models simultaneously on separate NPU cores
- **Zero-copy data pipelines** for minimal memory bandwidth usage
- **Sub-15ms inference latency** per model at full resolution

**Performance Advantages:**
- **3.3x more power-efficient** than Python implementations on identical hardware
- **1.7-2.2x lower power consumption** than competing edge AI platforms
- **Memory-efficient design** enables true 3-model parallel execution (impossible with Python on same hardware)
- **0.42 TOPS/Watt** computational efficiency vs. 0.17-0.28 for competitors

## BrightShopper: Flagship Application

**BrightShopper** is the first product built on this NPU platform, transforming BrightSign digital signage players into intelligent shopper analytics sensors.

### What BrightShopper Does

Real-time retail analytics running entirely on-device:
- **People counting & tracking** - Know exactly who's in view and where they move
- **Gaze detection & attention tracking** - Measure which content captures attention and for how long
- **Pose estimation** - 17-keypoint skeleton tracking for behavior recognition
- **Shopping behavior detection** - Cart pushing, basket carrying, shelf interaction, product selection
- **Content attribution** - Link engagement metrics to specific media (BSN.cloud Analytics)

### Unique Selling Propositions

1. **Edge-first, privacy-first**: All processing on-device. No video transmitted to cloud. Zero PII collected.

2. **Real-time performance**: Millisecond-latency analytics enable immediate content adaptation and live dashboards.

3. **Unmatched efficiency**: 5.5W for full 3-model analytics42% less power than competitors. Enables fanless 24/7 operation.

4. **Proven reliability**: Built on BrightSign's media player platform deployed in 100,000+ locations worldwide. 11-year expected lifespan vs. 2.5-3 years for fan-cooled alternatives.

5. **Multi-model intelligence**: Combines face detection (RetinaFace), pose estimation (YOLOv8-pose), and object detection (YOLOx) running in parallel for comprehensive behavioral insights.

6. **Content-to-engagement attribution**: BSN.cloud Analytics automatically links viewer metrics to specific media assets, proving campaign ROI.

### Platform Configurations

| Player | SoC | NPU Cores | Models Supported | Features |
|--------|-----|-----------|-----------------|----------|
| **XT5** | RK3588 | 3-core (6 TOPS) | 3 models parallel | Face + Pose + Object detection - full behavioral analytics |
| **XS156** | RK3576 | 2-core (4 TOPS) | 2 models parallel | Face + Object detection - engagement analytics |
| **LS5/HS5** | RK3568 | 1-core (1 TOPS) | 1 model | Face detection only - basic engagement metrics |

**Power consumption:**
- **XT5**: ~5.5W (3-core workload)
- **XS156**: ~4W estimated (2-core workload)
- **LS5/HS5**: ~3.5W estimated (1-core workload)

All configurations use the same optimized C++ implementation and share the core software architecture. Lower-tier products simply run fewer models in parallel based on available NPU cores.

## Documentation

- **[BrightShopper PR-FAQ](docs/bright-shopper-faq.md)** - Product vision, capabilities, and frequently asked questions
- **[Power Analysis](docs/power-analysis.md)** - Detailed efficiency, thermal, and reliability analysis
- **[Single-Core Power Comparison](docs/one-core-power-compare.md)** - XT5 vs. OrangePi vs. Raspberry Pi benchmarks
- **[Multi-Model Architecture](docs/multiple-models.md)** - Technical design for parallel NPU execution
- **[Design Overview](docs/design.md)** - System architecture and implementation details
- **[RGB-D Support](docs/rgbd.md)** - Depth sensing capabilities

## Key Metrics

| Metric | XT5 Value | Industry Context |
|--------|-----------|------------------|
| **TOPS/Watt** | 0.42 | 1.5-2.3x better than competitors |
| **R-TOPS/Watt** | 0.84 | 7-8x better lifetime computational value |
| **Power (3-core)** | 5.5W | 42-54% less than competitors |
| **MTBF** | 100,000 hrs | 3.6-4.5x longer than fan-cooled platforms |
| **Incremental power/core** | +0.80W | 3.3x better than Python on same hardware |

**R-TOPS/W** (Reliable-TOPS/Watt) = a novel metric combining computational efficiency (TOPS/W) with operational lifetime (MTBF), measuring the total computational value delivered per watt over the device's lifespan.

## Competitive Advantages

### vs. Cloud-based Analytics
- **Zero latency**: Milliseconds vs. seconds for cloud round-trip
- **No bandwidth costs**: All processing local, no video upload
- **100% uptime**: Works without internet connectivity
- **Privacy compliance**: No PII leaves the device

### vs. Python Edge AI Implementations
- **3.3x lower power per NPU core**: C++ vs. Python efficiency
- **Memory bandwidth**: Python would saturate NPU bandwidth at 3 models; C++ enables true parallelism
- **Production reliability**: Compiled binary vs. interpreter dependencies

### vs. External NPU Accelerators (Hailo, Coral)
- **1.8x lower total power**: No USB/PCIe overhead, native NPU integration
- **Lower baseline power**: 3.1W vs. 4.7W static
- **Simpler design**: No external modules, PoE-powered single-box solution

### vs. Retail Analytics Cameras
- **Dual-purpose hardware**: Digital signage + analytics in one device
- **Lower TCO**: No separate camera infrastructure
- **Content attribution**: Link viewer data to displayed media automatically

## Getting Started

*Documentation for deploying BrightShopper and developing custom NPU applications coming soon.*

## License

*License information to be determined.*

---

**BrightSign** - Market leader in digital signage media players
**Platform**: Rockchip RK3588 NPU-accelerated edge AI
**Applications**: Retail analytics, audience measurement, behavioral intelligence
