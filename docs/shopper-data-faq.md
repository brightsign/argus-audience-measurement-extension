# BrightShopper: Real-Time Shopper Analytics Platform
## Press Release / Frequently Asked Questions (PR-FAQ)

---

## Press Release

**FOR IMMEDIATE RELEASE**

### BrightSign Unveils BrightShopper: AI-Powered Real-Time Shopper Analytics Platform

*Groundbreaking edge AI solution delivers unprecedented insights into customer behavior, engagement, and shopping patterns without cloud dependency or privacy concerns*

**LOS GATOS, CA** – BrightSign, the global market leader in digital signage media players, today announced BrightShopper, a revolutionary edge AI analytics platform that transforms retail digital signage displays into intelligent customer behavior sensors. Built on BrightSign's proven hardware platform with the powerful RK3588 NPU processor, BrightShopper delivers real-time shopper analytics with zero cloud latency, complete data privacy, and cinema-quality performance.

BrightShopper represents a breakthrough in retail analytics by simultaneously running three specialized AI models across all three NPU cores of the RK3588 processor, delivering comprehensive behavioral insights that were previously impossible to achieve at the edge:

- **People counting and tracking**: Know exactly how many shoppers are in view and track their movement patterns
- **Facial detection and gaze tracking**: Understand which content captures attention and for how long
- **Pose estimation and behavior recognition**: Identify shopping behaviors like cart pushing, basket carrying, shelf interaction, and product selection

"Retailers have long struggled with the fundamental question: 'Who's watching my content, and are they engaged?'" said Anthony Gaudiosi, CEO of BrightSign. "BrightShopper answers this question in real-time with unprecedented detail, all while keeping customer data private and secure on the device. This is the future of retail analytics—powerful AI that respects privacy while delivering actionable insights."

Unlike cloud-based analytics solutions that introduce latency, raise privacy concerns, and incur ongoing bandwidth costs, BrightShopper processes all data locally on the BrightSign player. Retailers receive rich behavioral insights in milliseconds, not seconds, enabling real-time content adaptation and immediate performance measurement.

**Key Capabilities:**

- **Real-time engagement metrics**: Person count, face count, gaze count, and attention duration
- **Movement analytics**: Track shopping paths, dwell times, and traffic patterns
- **Behavioral insights**: Detect cart pushing, basket carrying, product interaction, and browsing behavior
- **Frame-to-frame tracking**: Maintain continuity across frames to understand customer journeys
- **Privacy-first design**: All processing on-device with no cloud transmission of video or personal data

BrightShopper is available immediately as a software upgrade for compatible BrightSign players with RK3588 processors. Custom behavior models and integration services are available through BrightSign's professional services team.

For more information, visit **brightsign.biz/brightshopper** or contact your BrightSign sales representative.

**About BrightSign**
BrightSign is the global market leader in digital signage media players, delivering reliable, high-performance solutions to over 100,000 customers worldwide. BrightSign players power digital signage in retail stores, restaurants, corporate environments, transportation hubs, and entertainment venues across the globe.

---

## Frequently Asked Questions

### Product Vision & Strategy

**Q: What is BrightShopper?**

A: BrightShopper is an edge AI analytics platform that transforms BrightSign digital signage players into intelligent shopper behavior sensors. It uses three specialized AI models running simultaneously on the RK3588's three NPU cores to deliver real-time insights about customer count, engagement, movement, and shopping behavior—all without requiring cloud connectivity or compromising privacy.

**Q: Why did BrightSign build BrightShopper?**

A: Retailers consistently tell us they need better data about who's seeing their content and whether it's working. Traditional solutions either require expensive infrastructure, raise privacy concerns with cloud-based video analysis, or provide only basic metrics like "someone walked by." BrightShopper delivers rich behavioral analytics that answer the fundamental questions: How many people are watching? Are they engaged? What are they doing? And it does this while keeping all data private and on-device.

**Q: Who is the target customer?**

A: BrightShopper is designed for:
- **Retailers** wanting to measure digital signage ROI and optimize content placement
- **Brands** deploying in-store marketing displays who need engagement metrics
- **Shopping centers and malls** seeking foot traffic and dwell time analytics
- **QSRs and convenience stores** wanting to understand queue behavior and product interaction
- **Consumer goods companies** running promotional displays who need proof of engagement

**Q: How is BrightShopper different from existing analytics solutions?**

A: Most analytics solutions face a fundamental tradeoff between capability, privacy, and performance:

- **Simple people counters** provide basic counts but no behavioral insight
- **Cloud-based video analytics** offer rich analytics but raise privacy concerns, require bandwidth, and introduce latency
- **Retail analytics platforms** require dedicated hardware, complex installation, and ongoing services

BrightShopper eliminates these tradeoffs by:
- Running all AI processing on-device (no video leaves the player)
- Delivering comprehensive behavioral analytics from a single device
- Requiring no additional hardware beyond the existing BrightSign player
- Providing real-time results in under 70ms
- Operating with no recurring cloud costs

### Technical Capabilities

**Q: What specific metrics does BrightShopper provide?**

A: BrightShopper delivers frame-by-frame analytics including:

**Counts (instantaneous):**
- Number of people in frame
- Number of faces detected
- Number of people looking at screen

**Tracking (temporal):**
- Unique person IDs maintained across frames
- Movement direction and velocity
- Dwell time and path tracking
- Gaze state changes (when people look toward/away)

**Behavioral insights:**
- Shopping with cart or basket
- Browsing/standing behavior
- Walking/moving through space
- Shelf interaction and product pickup
- Pose changes indicating specific actions

**Q: How accurate is BrightShopper?**

A: BrightShopper uses state-of-the-art AI models optimized for retail environments:
- **Person detection**: >95% precision at typical retail distances (2-15 feet)
- **Face detection**: >90% detection rate for faces at 1-10 feet
- **Gaze estimation**: ±15-degree accuracy for determining screen attention
- **Pose keypoints**: 17-point skeleton with >85% keypoint detection rate
- **Tracking**: Maintains ID consistency >95% of the time in typical retail scenarios

Accuracy varies with lighting, camera angle, distance, and occlusion. We provide best practices guides for camera placement to optimize performance.

**Q: What AI models power BrightShopper?**

A: BrightShopper runs three AI models simultaneously, each on its own NPU core:

1. **RetinaFace** (NPU Core 0): Face detection and gaze estimation - identifies faces and determines if people are looking at the screen
2. **YOLOv8-pose** (NPU Core 1): Person detection with 17-point pose estimation - tracks body position and enables behavior recognition
3. **YOLOx** (NPU Core 2): Object detection - identifies shopping carts, baskets, and products for context

These models work together through a fusion layer that correlates detections, then a tracking layer maintains person identity and behavior state across frames.

**Q: What is the performance and latency?**

A: BrightShopper achieves real-time performance:
- **Latency**: ~69ms end-to-end (capture to analytics output)
- **Throughput**: 14 FPS with all three models, 20+ FPS with optimized YOLOx variant
- **All 3 NPU cores utilized**: Maximum efficiency from RK3588 hardware

This real-time performance enables immediate content adaptation and responsive interactive experiences.

**Q: What hardware is required?**

A: BrightShopper requires:
- **BrightSign player** with RK3588 NPU (RK3588-based models)
- **USB or IP camera** (720p minimum, 1080p recommended)
- **Network connection** for analytics output (UDP/JSON, HTTP, MQTT supported)

No additional compute hardware or cloud services required. The BrightSign player handles all AI processing.

**Q: How does BrightShopper handle privacy?**

A: Privacy is built into BrightShopper's architecture:

- **No video storage**: Raw video is never saved to disk
- **No cloud transmission**: Video never leaves the device
- **On-device processing**: All AI inference happens locally on the NPU
- **Anonymized output**: Only pose keypoints, bounding boxes, and counts are transmitted—no images or facial recognition
- **GDPR/CCPA friendly**: No personal data is collected or stored
- **Configurable privacy zones**: Option to disable analytics in specific areas

Retailers get rich behavioral insights without compromising customer privacy.

### Business Model & Pricing

**Q: How is BrightShopper priced?**

A: BrightShopper is available through flexible licensing:

- **Software license** included with compatible RK3588-based BrightSign players
- **Analytics tier pricing** based on output frequency and feature set:
  - **Basic**: Person/face counts, gaze detection ($X/player/month)
  - **Professional**: Adds tracking, movement analytics ($X/player/month)
  - **Enterprise**: Adds behavior detection, custom models (custom pricing)

- **One-time purchase** option available for high-volume deployments
- **Professional services** available for custom model training and integration

Contact your BrightSign sales representative for detailed pricing.

**Q: What is included in the base price vs. add-ons?**

A: **Base BrightShopper license includes:**
- Real-time person and face counting
- Gaze detection and engagement tracking
- Basic movement tracking
- JSON/UDP output
- Standard AI models

**Professional tier adds:**
- Extended tracking with behavior history
- Movement pattern analysis
- Dwell time analytics
- HTTP/MQTT output
- Dashboard visualization

**Enterprise tier adds:**
- Custom behavior model training (cart pushing, product pickup, etc.)
- Transfer learning for store-specific actions
- Advanced API integration
- Priority support and SLAs

**Q: What is the ROI for retailers?**

A: Retailers see ROI through multiple channels:

**Content optimization**: Test which creative drives engagement, optimize content rotation schedules, and eliminate underperforming assets (typical 20-40% improvement in engagement rates)

**Placement optimization**: Understand which display locations drive the most attention and interaction (helps justify premium placement costs or relocate underperforming displays)

**Campaign measurement**: Provide advertisers and brands with engagement proof-of-performance (enables premium pricing for advertising inventory)

**Operational insights**: Understand traffic patterns, peak times, and customer flows to optimize staffing and merchandising

**Competitive advantage**: Offer data-driven insights that pure digital signage competitors cannot match

### Integration & Deployment

**Q: How does BrightShopper integrate with existing systems?**

A: BrightShopper provides flexible integration options:

**Real-time outputs:**
- **UDP JSON**: Low-latency streaming for real-time applications
- **HTTP POST**: Standard REST API for easy integration
- **MQTT**: Pub/sub for IoT platforms and analytics dashboards

**Analytics platforms:**
- **Pre-built connectors**: Google Analytics, Tableau, Power BI
- **Custom webhooks**: Send to any HTTP endpoint
- **Data warehouse export**: Batch export for historical analysis

**Content management systems:**
- **BrightAuthor integration**: Trigger content changes based on analytics
- **Third-party CMS**: API for integration with any CMS platform

**Q: What does deployment look like?**

A: BrightShopper deployment is straightforward:

1. **Hardware setup**: Install BrightSign player with RK3588 and connect camera (same as standard digital signage deployment)
2. **Software configuration**: Enable BrightShopper via BrightAuthor or configuration file, set camera parameters and privacy zones
3. **Model deployment**: Download AI models to player (included with license, ~15MB total)
4. **Integration setup**: Configure analytics output (UDP, HTTP, or MQTT endpoints)
5. **Validation**: Use provided dashboard to verify analytics are flowing correctly

Typical deployment time: 15-30 minutes per player. Remote deployment and configuration supported.

**Q: Can I customize the behavior detection for my store?**

A: Yes! BrightShopper supports custom behavior models through transfer learning:

**Out-of-box behaviors:**
- Cart pushing / basket carrying
- Shelf interaction / product reach
- Standing / browsing
- Walking / moving

**Custom behaviors** (via transfer learning):
- Store-specific product interactions
- Queue management behaviors
- Fitting room entrances
- Service counter interactions
- Any behavior visible through pose + context

BrightSign Professional Services can help train custom models using your store footage.

### Competition & Market Position

**Q: How does BrightShopper compare to RetailNext, ShopperTrak, etc.?**

A: Traditional retail analytics platforms require dedicated hardware installations:

| Feature | BrightShopper | Traditional Analytics | Cloud Video Analytics |
|---------|---------------|----------------------|----------------------|
| **Hardware cost** | $0 (uses existing signage player) | $500-2000+ per location | $0 (software only) |
| **Installation** | 15 min (camera only) | 2-4 hours (dedicated sensors) | 15 min (camera only) |
| **Privacy** | On-device, no video leaves player | Varies by vendor | Video sent to cloud |
| **Latency** | <70ms real-time | Seconds to minutes | Seconds (round-trip) |
| **Behavioral insights** | Rich (pose + gaze + objects) | Basic (counts + zones) | Rich (video analysis) |
| **Ongoing costs** | Software license only | Hardware + software + services | Bandwidth + compute + storage |
| **Integration** | Built into signage CMS | Separate platform | Separate platform |

BrightShopper delivers comparable or superior analytics at a fraction of the cost and complexity.

**Q: Why not just use a smartphone app or WiFi tracking?**

A: Smartphone-based solutions have fundamental limitations:

- **Opt-in required**: Only tracks customers who download app and enable tracking (typically <5% of shoppers)
- **No behavioral insight**: Can't see what people are looking at, holding, or doing
- **No content correlation**: Can't tie analytics to specific digital signage content
- **Privacy concerns**: Tracking phone IDs raises more privacy issues than anonymous pose data

BrightShopper provides 100% coverage of everyone in the camera's view, with rich behavioral context, and better privacy protection.

### Future Roadmap

**Q: What's next for BrightShopper?**

A: Our roadmap includes:

**Near-term (6 months):**
- Demographic estimation (age range, gender - optional, privacy-configurable)
- Emotion detection (engaged, surprised, confused)
- Group detection (shopping alone vs. with family)
- Extended tracking across multiple cameras

**Medium-term (12 months):**
- Product recognition (identify specific products being picked up)
- Gesture recognition (pointing, reaching, waving)
- Queue analytics (line length, wait times)
- Heat map generation (attention density maps)

**Long-term (18+ months):**
- Multi-modal sensing (audio + visual for richer context)
- Predictive analytics (predict likely purchase intent)
- Real-time content adaptation (automatically optimize content based on audience)
- Cross-location analytics (compare performance across stores)

**Q: Will BrightShopper work with future BrightSign hardware?**

A: Yes. BrightShopper is designed to scale with future hardware:
- Compatible with current and future RK3588-based players
- Modular architecture supports next-gen NPUs and AI accelerators
- Model format (RKNN) is forward-compatible with Rockchip roadmap
- Software updates ensure compatibility with new BrightSign OS versions

As hardware improves, BrightShopper will automatically deliver higher frame rates, support more simultaneous tracks, and enable more sophisticated models—all through software updates.

---

## Customer Testimonials

*"BrightShopper has transformed how we measure our in-store digital marketing ROI. We can finally prove which content drives engagement and optimize our creative accordingly. The insights we're getting are incredible, and the fact that it's all private and on-device means we have no concerns about customer data."*
— **Jamie Rodriguez, Director of Digital Marketing, [Major Retail Chain]**

*"We deployed BrightShopper across 200 stores in 30 days. The installation was simple—just connect a camera—and the analytics started flowing immediately. We're now using dwell time and engagement metrics to optimize product placement and justify premium advertising rates. This is a game-changer."*
— **Michael Chen, VP of Store Operations, [National Grocery Chain]**

*"As a brand running promotional displays in retail, BrightShopper gives us the proof-of-performance data we need to show our CPG clients. We can demonstrate not just foot traffic, but actual engagement and interaction. It's become a key differentiator in our sales process."*
— **Sarah Mitchell, CEO, [Retail Marketing Agency]**
