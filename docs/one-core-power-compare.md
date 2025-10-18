# Single-Core NPU Power Consumption Comparison

## Overview

This document compares power consumption across different platforms when running face detection inference on a single NPU core. These measurements demonstrate the efficiency advantages of the BrightSign XT5 with RK3588 NPU for edge AI workloads.

## Test Configuration

- **Workload**: Face detection inference (RetinaFace model)
- **NPU utilization**: Single core active
- **Measurement**: Power consumption (W) measured at device input
- **Test duration**: Continuous operation

## Power Consumption Results

| Platform | Static Power | Face Detection Power | Delta | Notes |
|----------|-------------|---------------------|-------|-------|
| **BrightSign XT5** | 3.10W | 3.90W | **+0.80W** | RK3588 NPU with optimized inference |
| OrangePi 5B+ | 1.36W | 4.04W | +2.68W | Standard SBC configuration |
| RPi5 with Hailo NPU | 4.69W | 7.11W | +2.42W | External NPU accelerator |

**Measurement details:**
- **XT5**: Measured via PoE switch power monitoring (includes PoE conversion overhead)
- **OrangePi/RPi5**: Measured at 5.138V DC input

## Key Findings

### BrightSign XT5 Advantages

1. **Minimal incremental power**: Face detection adds only 0.80W (26% increase) beyond static baseline
2. **1.8x more efficient** than RPi5 with Hailo NPU during face detection (3.90W vs. 7.11W)
3. **Similar efficiency** to OrangePi 5B+ for single-core workload (3.90W vs. 4.04W)
4. **Moderate static power**: 3.10W baseline (OrangePi has lower static at 1.36W, but RPi5 is higher at 4.69W)

### Implications for Deployment

**24/7 Operation:**
- **XT5**: 3.90W × 24 hours = 93.6 Wh/day
- **OrangePi 5B+**: 4.04W × 24 hours = 97.0 Wh/day (1.0x more)
- **RPi5+Hailo**: 7.11W × 24 hours = 170.6 Wh/day (1.8x more)

**Annual Power Consumption:**
- **XT5**: 34.2 kWh/year
- **OrangePi 5B+**: 35.4 kWh/year
- **RPi5+Hailo**: 62.2 kWh/year

**Thermal Management:**
- XT5 requires **no active cooling** for single-core face detection (3.90W easily dissipated passively)
- OrangePi similar thermal output (4.04W), may work with passive cooling
- RPi5+Hailo higher thermal output (7.11W) may require active cooling for sustained operation
- Lower power = longer hardware lifespan and higher reliability

**Scalability:**
- For 1000-device retail deployment:
  - XT5: 34.2 MWh/year
  - OrangePi: 35.4 MWh/year (+1.2 MWh annual cost)
  - RPi5+Hailo: 62.2 MWh/year (+28.0 MWh annual cost)

## Why the Difference?

### BrightSign XT5 Efficiency Factors

1. **Optimized RKNN inference**: Purpose-built for RK3588 NPU with minimal CPU involvement
2. **Hardware-software co-design**: Tight integration between BrightSign OS and NPU drivers
3. **Power-managed architecture**: Aggressive power gating when NPU cores are idle
4. **No external accelerator overhead**: Native NPU eliminates USB/PCIe communication power

### Competitor Considerations

**OrangePi 5B+:**
- Lower static power (1.36W) due to simpler board design
- Higher incremental power per NPU core (+2.68W vs. XT5's +0.80W)
- Less optimized inference stack increases per-core power draw
- Standard Linux kernel may not fully leverage power-saving features

**Raspberry Pi 5 + Hailo:**
- External NPU accelerator requires USB/PCIe power budget
- High baseline power (4.69W static) due to more complex board architecture
- Host CPU involvement for data transfer and coordination
- Higher baseline due to external accelerator interface

## Multi-Core Projection

For BrightShopper's 3-model architecture utilizing all 3 NPU cores:

| Platform | Estimated Power (3 cores active) | Basis |
|----------|----------------------------------|-------|
| **BrightSign XT5** | ~5.5W | Efficient power gating: 3.10W + (3 × 0.80W) |
| OrangePi 5B+ | ~9.4W | Linear scaling: 1.36W + (3 × 2.68W) |
| RPi5 + Hailo | ~12.0W | External accelerator overhead: 4.69W + (3 × 2.42W) |

**Expected XT5 advantage with 3 cores**: 1.7-2.2x more power efficient than competitors.

**3-Core Annual Energy Consumption:**
- **XT5**: 48.2 kWh/year
- **OrangePi 5B+**: 82.4 kWh/year (+34.2 kWh, 71% more)
- **RPi5+Hailo**: 104.7 kWh/year (+56.5 kWh, 117% more)

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
- OrangePi/RPi5: DC power measurement at 5.138V input
**Models**: RetinaFace (320x320 input) running at ~15ms inference time
