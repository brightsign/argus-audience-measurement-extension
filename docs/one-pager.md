# BrightShopper: Edge AI Retail Analytics Platform

## The Elevator Pitch

BrightShopper transforms BrightSign digital signage players into intelligent retail analytics sensors. It answers the fundamental question retailers ask: "Who's watching my content, and are they engaged?" Using on-device AI models, BrightShopper delivers real-time behavioral insights—people counting, gaze tracking, pose estimation, shopping behaviors—without cloud dependency, bandwidth costs, or privacy concerns. At 5.5W power consumption with 11-year expected lifespan, it's the most efficient and reliable edge AI solution for retail analytics.

## Value Proposition

### Dual-Purpose Hardware, Single Infrastructure Cost
Unlike dedicated analytics cameras requiring separate installation and management, BrightShopper runs on the same BrightSign player delivering your digital signage content. One device, one power connection, one management interfacedelivering both content and analytics. This eliminates the need for separate camera infrastructure, reducing capital costs and installation complexity while enabling unique content attribution capabilities that link viewer engagement directly to displayed media assets.

### Privacy-First Edge Processing
All AI processing occurs on-device. No video leaves the player. No personally identifiable information is collected or transmitted. BrightShopper delivers rich behavioral insightsperson counts, gaze patterns, dwell times, shopping behaviorswhile maintaining complete data privacy. This edge-first architecture eliminates privacy concerns associated with cloud-based video analytics, ensures compliance with data protection regulations, and removes ongoing bandwidth costs.

### Real-Time Performance at Ultra-Low Power
BrightShopper delivers millisecond-latency analytics enabling immediate content adaptation and live dashboards. The C++ implementation on Rockchip NPU hardware achieves 0.42 TOPS/Watt efficiency3.3x better than Python implementations on identical hardware and 1.5-2.3x better than competing platforms. At just 5.5W for full 3-model operation (XT5), the platform enables fanless 24/7 deployment with 100,000-hour MTBF (11-year expected lifespan) versus 2.5-3 years for fan-cooled alternatives.

### Scalable Platform Architecture
Three product tiers serve different deployment needs while sharing the same software architecture:
- **XT5 (RK3588, 3-core NPU)**: Full behavioral analytics with pose estimation and behavior recognition
- **XS156 (RK3576, 2-core NPU)**: Core engagement metrics with face detection and object tracking
- **LS5/HS5 (RK3568, 1-core NPU)**: Fundamental attention tracking for budget-conscious deployments

### Proven Economics
For 1,000-unit deployments over 5 years, BrightShopper delivers $62,000-$100,000 operational savings versus competing platforms through reduced power consumption (42-54% less), zero fan replacement costs, and minimal device failures (<2% vs. 20-30% for fan-cooled alternatives). The platform's 0.84 R-TOPS/Watt (Reliable-TOPS/Watt) metric demonstrates 7-8x better lifetime computational value than alternatives, accounting for both efficiency and longevity.

### Content-to-Engagement Attribution
When integrated with BSN.cloud Analytics, BrightShopper automatically correlates viewer engagement data with the specific content displayed at that moment. Retailers can measure which creative assets drive attention, optimize scheduling based on time-of-day performance, conduct A/B testing with real engagement data, and prove campaign ROI with metrics like "Creative A achieved 45% gaze rate versus Creative B's 28%." This closed-loop measurement transforms digital signage from a broadcast medium into a measurable, optimizable marketing channel.

---

**BrightSign NPU Platform** | Retail Analytics | Audience Measurement | Behavioral Intelligence
*Built on 100,000+ BrightSign players deployed worldwide*
