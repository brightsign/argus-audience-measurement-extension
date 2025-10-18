# BrightSign NPU Platform - Executive Summary Slides

---

## Slide 1: BrightSign NPU Platform
### Next-Generation Edge AI for Digital Signage

**Three-Tier Platform Architecture**

| Tier | Player | NPU | Capabilities |
|------|--------|-----|--------------|
| **Premium** | XT5 | RK3588 (3-core, 6 TOPS) | 3 models parallel - Full behavioral analytics |
| **Mid-tier** | XS156 | RK3576 (2-core, 4 TOPS) | 2 models parallel - Core engagement metrics |
| **Entry** | LS5/HS5 | RK3568 (1-core, 1 TOPS) | 1 model - Basic attention tracking |

**Built on proven BrightSign reliability:**
- Fanless operation, 11-year lifespan (100,000-hour MTBF)
- 5.5W full AI workload (XT5) - 42-54% less power than competitors
- C++ optimized implementation - 3.3x more efficient than Python alternatives

---

## Slide 2: BrightShopper - Flagship Application
### Real-Time Retail Analytics at the Edge

**What It Does:**
- **People counting & tracking** - Movement patterns and traffic flow
- **Gaze detection & attention** - What content captures attention, for how long
- **Pose estimation (XT5)** - 17-keypoint skeleton tracking
- **Shopping behaviors** - Cart pushing, basket carrying, shelf interaction, product selection
- **Content attribution** - Link engagement to specific media assets (BSN.cloud)

**How It's Different:**
-  **Edge-first, privacy-first** - All processing on-device, zero PII transmitted
-  **Real-time performance** - Millisecond latency vs. seconds for cloud
-  **Dual-purpose hardware** - Digital signage + analytics in one device
-  **No cloud costs** - No bandwidth, no recurring fees

---

## Slide 3: Competitive Advantages
### Why BrightSign NPU Platform Wins

**vs. Cloud-Based Analytics**
- **Zero latency**: Milliseconds vs. seconds for round-trip
- **Privacy compliance**: No video leaves device, no PII collected
- **100% uptime**: Works without internet connectivity

**vs. Python Edge AI Implementations**
- **3.3x lower power per NPU core**: C++ vs. Python efficiency
- **Memory bandwidth**: Python saturates NPU at 3 models; C++ enables true parallelism
- **Production reliability**: Compiled binary vs. interpreter dependencies

**vs. External NPU Accelerators (Hailo, Coral, etc.)**
- **1.8x lower total power**: No USB/PCIe overhead, native integration
- **Simpler design**: PoE-powered single-box solution

**vs. Dedicated Retail Analytics Cameras**
- **Lower TCO**: No separate camera infrastructure
- **Content attribution**: Automatic linkage of viewer data to displayed media

**Key Metrics (XT5):**
- **0.42 TOPS/Watt** - 1.5-2.3x better computational efficiency
- **0.84 R-TOPS/Watt** - 7-8x better lifetime computational value
- **$62K-$100K savings** over 5 years per 1,000 units deployed
