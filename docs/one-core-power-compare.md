# Single-Core NPU Power Consumption Comparison

## Overview

This document compares power consumption across different platforms when running face detection inference on a single NPU core. These measurements demonstrate the efficiency advantages of BrightSign's NPU platform for edge AI workloads.

**Platform Context**: BrightSign offers NPU-enabled players across three Rockchip SoCs:
- **XT5**: RK3588 (3-core NPU, 6 TOPS)
- **XS156**: RK3576 (2-core NPU, 4 TOPS)
- **LS5/HS5**: RK3568 (1-core NPU, 1 TOPS)

This analysis focuses on the **XT5 (RK3588)** as the reference platform. The per-core efficiency characteristics apply equally to XS156 and LS5/HS5, which use the same C++ implementation but run fewer models based on available NPU cores.

## Test Configuration

- **Workload**: Face detection inference (RetinaFace model)
- **NPU utilization**: Single core active
- **Measurement**: Power consumption (W) measured at device input
- **Test duration**: Continuous operation

## Power Consumption Results

| Platform | Static Power | Face Detection Power | Delta | Notes |
|----------|-------------|---------------------|-------|-------|
| **BrightSign XT5** | 3.10W | 3.90W | **+0.80W** | RK3588 NPU with optimized inference |
| OrangePi 5B+ | 1.37W | 4.06W | +2.69W | Standard SBC configuration |
| RPi5 with Hailo NPU | 4.71W | 7.14W | +2.43W | External NPU accelerator |

**Measurement details:**
- **XT5**: Measured via PoE switch power monitoring (includes PoE conversion overhead)
- **OrangePi/RPi5**: Measured at 5.160V DC input

## Key Findings

### BrightSign XT5 Advantages

1. **Minimal incremental power**: Face detection adds only 0.80W (26% increase) beyond static baseline
2. **1.8x more efficient** than RPi5 with Hailo NPU during face detection (3.90W vs. 7.14W)
3. **Similar efficiency** to OrangePi 5B+ for single-core workload (3.90W vs. 4.06W)
4. **Moderate static power**: 3.10W baseline (OrangePi has lower static at 1.37W, but RPi5 is higher at 4.71W)

### Implications for Deployment

**24/7 Operation:**
- **XT5**: 3.90W × 24 hours = 93.6 Wh/day
- **OrangePi 5B+**: 4.06W × 24 hours = 97.0 Wh/day (1.0x more)
- **RPi5+Hailo**: 7.14W × 24 hours = 170.6 Wh/day (1.8x more)

**Annual Power Consumption:**
- **XT5**: 34.2 kWh/year
- **OrangePi 5B+**: 35.6 kWh/year
- **RPi5+Hailo**: 62.5 kWh/year

**Thermal Management:**
- XT5 requires **no active cooling** for single-core face detection (3.90W easily dissipated passively)
- OrangePi similar thermal output (4.06W), may work with passive cooling
- RPi5+Hailo higher thermal output (7.14W) may require active cooling for sustained operation
- Lower power = longer hardware lifespan and higher reliability

**Scalability:**
- For 1000-device retail deployment:
  - XT5: 34.2 MWh/year
  - OrangePi: 35.6 MWh/year (+1.4 MWh annual cost)
  - RPi5+Hailo: 62.5 MWh/year (+28.3 MWh annual cost)

## Why the Difference?
### Platform Architecture Context

**Important Note**: The XT5 and OrangePi 5B+ both use the **same RK3588 processor** with identical NPU hardware (3-core, 6 TOPS total). The power consumption differences are primarily due to software implementation and system design:

**Software Implementation:**
- **XT5**: BrightShopper implemented in **C++** with optimized RKNN runtime
- **OrangePi 5B+**: Reference implementation in **Python** with standard RKNN bindings
- **RPi5+Hailo**: Python implementation with external NPU accelerator

**Expected Behavior:**
- **Static power**: XT5 slightly higher (3.10W vs 1.37W) due to additional control plane software running on BrightSign OS
- **Per-core inference power**: The large difference (+0.80W for XT5 vs +2.69W for OrangePi) reflects **C++ vs Python efficiency**
- **Same hardware**: Both platforms use identical RK3588 NPU cores, so hardware efficiency is the same
- **Software overhead**: Python interpreter overhead, memory management, and less optimized data pipelines account for 3.3x higher incremental power per NPU core on OrangePi

**Memory Bandwidth Limitation**: Python's inefficient memory management (object overhead, reference counting, fragmented allocations) significantly increases memory bandwidth requirements. For the 3-model BrightShopper workload (RetinaFace + YOLOv8-pose + YOLOx), Python's memory overhead would likely **exceed the RK3588 NPU's memory bandwidth capacity**, making simultaneous 3-model operation impractical or impossible without severe performance degradation. This is why the multi-core projection for OrangePi should be considered theoretical - actual deployment would require sequential model execution rather than parallel.

With a C++ implementation on OrangePi 5B+, we would expect performance nearly identical to XT5 (within 10-15% due to OS differences).


### BrightSign XT5 Efficiency Factors

1. **Optimized RKNN inference**: Purpose-built for RK3588 NPU with minimal CPU involvement
2. **Hardware-software co-design**: Tight integration between BrightSign OS and NPU drivers
3. **Power-managed architecture**: Aggressive power gating when NPU cores are idle
4. **No external accelerator overhead**: Native NPU eliminates USB/PCIe communication power

### Competitor Considerations

**OrangePi 5B+:**
- Lower static power (1.37W) due to simpler board design
- Higher incremental power per NPU core (+2.69W vs. XT5's +0.80W)
- Less optimized inference stack increases per-core power draw
- Standard Linux kernel may not fully leverage power-saving features

**Raspberry Pi 5 + Hailo:**
- External NPU accelerator requires USB/PCIe power budget
- High baseline power (4.71W static) due to more complex board architecture
- Host CPU involvement for data transfer and coordination
- Higher baseline due to external accelerator interface

## Multi-Core Projection

For BrightShopper's 3-model architecture utilizing all 3 NPU cores:

| Platform | Estimated Power (3 cores active) | Basis |
|----------|----------------------------------|-------|
| **BrightSign XT5** | ~5.5W | Efficient power gating: 3.10W + (3 × 0.80W) |
| OrangePi 5B+ | ~9.5W* | Linear scaling: 1.37W + (3 × 2.69W) |
| RPi5 + Hailo | ~12.00W | External accelerator overhead: 4.71W + (3 × 2.43W) |

**\*Important caveat**: The OrangePi 9.5W estimate assumes parallel 3-model execution is possible with Python. In practice, **Python's memory inefficiency would likely prevent running 3 models simultaneously** due to NPU memory bandwidth constraints. The Python implementation would need to run models sequentially, reducing throughput significantly.

**Expected XT5 advantage with 3 cores**: 1.7-2.2x more power efficient than competitors (assuming competitors can achieve 3-model parallelism, which is unlikely with Python).

**3-Core Annual Energy Consumption:**
- **XT5**: 48.2 kWh/year
- **OrangePi 5B+**: 82.8 kWh/year (+34.6 kWh, 71% more)
- **RPi5+Hailo**: 105.1 kWh/year (+56.9 kWh, 117% more)

## Conclusion

The BrightSign XT5 demonstrates exceptional power efficiency for edge AI workloads:
- **Minimal incremental power** for single-core face detection (+0.80W, only 26% increase from baseline)
- **1.8x lower power consumption** vs. RPi5+Hailo for single-core workload
- **1.7-2.2x more efficient** than competitors when running 3-model BrightShopper workload
- **Enables fanless, 24/7 deployment** without thermal concerns
- **Significant cost savings** for large-scale retail deployments

This efficiency advantage makes BrightSign the ideal platform for always-on retail analytics applications like BrightShopper.

---

**Test Date**: 2024
**Measurement Method**: 
- XT5: PoE switch power monitoring (includes PoE conversion overhead)
- OrangePi/RPi5: DC power measurement at 5.160V input
**Models**: RetinaFace (320x320 input) running at ~15ms inference time
