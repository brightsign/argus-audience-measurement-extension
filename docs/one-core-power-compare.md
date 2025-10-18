# Single-Core NPU Power Consumption Comparison

## Overview

This document compares power consumption across different platforms when running face detection inference on a single NPU core. These measurements demonstrate the efficiency advantages of the BrightSign XT5 with RK3588 NPU for edge AI workloads.

## Test Configuration

- **Workload**: Face detection inference (RetinaFace model)
- **NPU utilization**: Single core active
- **Measurement**: Current draw (mA) at standard operating voltage
- **Test duration**: Continuous operation

## Power Consumption Results

| Platform | Static Power | Face Detection Power | Delta | Notes |
|----------|-------------|---------------------|-------|-------|
| **BrightSign XT5** | 58 mA | 58 mA | **0 mA** | RK3588 NPU with optimized inference |
| OrangePi 5B+ | 265 mA | 787 mA | +522 mA | Standard SBC configuration |
| RPi5 with Hailo NPU | 912 mA | 1383 mA | +471 mA | External NPU accelerator |

## Key Findings

### BrightSign XT5 Advantages

1. **Zero incremental power**: Face detection adds no measurable power consumption beyond static baseline
2. **15x more efficient** than RPi5 with Hailo NPU during face detection (58 mA vs. 1383 mA)
3. **13.6x more efficient** than OrangePi 5B+ during face detection (58 mA vs. 787 mA)
4. **Lowest static power**: 58 mA baseline is 4.6x lower than OrangePi and 15.7x lower than RPi5+Hailo

### Implications for Deployment

**24/7 Operation:**
- **XT5**: 58 mA × 24 hours = 1.39 Ah/day
- **OrangePi 5B+**: 787 mA × 24 hours = 18.9 Ah/day (13.6x more)
- **RPi5+Hailo**: 1383 mA × 24 hours = 33.2 Ah/day (23.9x more)

**Annual Power Consumption (at 5V):**
- **XT5**: ~2.5 kWh/year
- **OrangePi 5B+**: ~34.5 kWh/year
- **RPi5+Hailo**: ~60.6 kWh/year

**Thermal Management:**
- XT5 requires **no active cooling** for single-core face detection
- Competitors may require heat sinks or active cooling for sustained operation
- Lower power = longer hardware lifespan and higher reliability

**Scalability:**
- For 1000-device retail deployment:
  - XT5: 2.5 MWh/year
  - OrangePi: 34.5 MWh/year (+32 MWh annual cost)
  - RPi5+Hailo: 60.6 MWh/year (+58.1 MWh annual cost)

## Why the Difference?

### BrightSign XT5 Efficiency Factors

1. **Optimized RKNN inference**: Purpose-built for RK3588 NPU with minimal CPU involvement
2. **Hardware-software co-design**: Tight integration between BrightSign OS and NPU drivers
3. **Power-managed architecture**: Aggressive power gating when NPU cores are idle
4. **No external accelerator overhead**: Native NPU eliminates USB/PCIe communication power

### Competitor Considerations

**OrangePi 5B+:**
- Higher baseline due to less optimized power management
- CPU involvement in inference pipeline increases power draw
- Standard Linux kernel may not fully leverage power-saving features

**Raspberry Pi 5 + Hailo:**
- External NPU accelerator requires USB/PCIe power budget
- Host CPU involvement for data transfer and coordination
- Higher baseline due to more complex board architecture

## Multi-Core Projection

For BrightShopper's 3-model architecture utilizing all 3 NPU cores:

| Platform | Estimated Power (3 cores active) | Basis |
|----------|----------------------------------|-------|
| **BrightSign XT5** | ~58-80 mA | Efficient power gating, minimal incremental cost |
| OrangePi 5B+ | ~1200-1500 mA | Linear scaling with core utilization |
| RPi5 + Hailo | ~2000-2500 mA | External accelerator + host coordination |

**Expected XT5 advantage with 3 cores**: Still 15-30x more power efficient than competitors.

## Conclusion

The BrightSign XT5 demonstrates exceptional power efficiency for edge AI workloads:
- **Zero incremental power** for single-core face detection
- **15x lower power consumption** vs. RPi5+Hailo
- **Enables fanless, 24/7 deployment** without thermal concerns
- **Significant cost savings** for large-scale retail deployments

This efficiency advantage makes BrightSign the ideal platform for always-on retail analytics applications like BrightShopper.

---

**Test Date**: 2024
**Measurement Method**: Direct current measurement under continuous face detection workload
**Models**: RetinaFace (320x320 input) running at ~15ms inference time
