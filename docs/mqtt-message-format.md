# MQTT Message Format - Analytics Output

> **Quick Start:** For a practical guide with code examples, see **[MQTT Integration Guide](INTEGRATION-MQTT.md)**.
>
> This document is the complete **schema reference** for MQTT messages.

## Overview

The Argus analytics system publishes real-time tracking data via MQTT to the topic `bs/argus/analytics`. This document explains the complete message structure and all parameters to help developers create dashboards, analytics tools, and monitoring applications.

---

## Message Structure

### Example Message

```json
{
  "schema": "analytics/v7.0",
  "ts": 164.68,
  "device": "XS-156",
  "stream": "rtsp://192.168.0.203:8554/live",
  "frame_w": 1280,
  "frame_h": 720,
  "model": "yolox_s",
  "fw_version": "7.0.0",
  "npu_load": 87.0,
  "people": 8,
  "people_confident": 6,
  "gaze": 1,
  "fps": 29,
  "roi": {
    "type": "border",
    "border_frac": 0.10,
    "rect": [128, 72, 1152, 648]
  },
  "health": {
    "detector_fps": 29.0,
    "tracker_fps": 29.0,
    "queue_latency_ms": 0,
    "dropped_frames": 0,
    "last_model_reload_ts": 0.0
  },
  "tracks": [
    {
      "id": 79,
      "state": "Confirmed",
      "bbox": [1017.3, 328.1, 1141.3, 526.7],
      "score": 0.53,
      "zones": ["roi"],
      "dir": "?",
      "deg": 0.0,
      "dir_conf": 0.00,
      "speed": 0.0,
      "speed_norm": 0.000,
      "dwell": 14.01,
      "enter": false,
      "exit": false
    },
    {
      "id": 63,
      "state": "Confirmed",
      "bbox": [52.9, 227.4, 286.6, 720.0],
      "score": 0.93,
      "zones": ["roi"],
      "dir": "?",
      "deg": 0.0,
      "dir_conf": 0.00,
      "speed": 0.0,
      "speed_norm": 0.000,
      "dwell": 31.03,
      "enter": false,
      "exit": false,
      "gaze": {
        "detected": 1,
        "time": 8.00,
        "face_bbox": [231.0, 167.0, 240.0, 180.0]
      }
    },
    {
      "id": 50,
      "state": "Confirmed",
      "bbox": [696.7, 278.1, 790.0, 699.9],
      "score": 0.53,
      "zones": ["roi"],
      "dir": "UR",
      "deg": 37.7,
      "dir_conf": 0.67,
      "speed": 4.5,
      "speed_norm": 0.003,
      "dwell": 77.07,
      "enter": false,
      "exit": false
    }
  ]
}
```

---

### JSON Schema

**Formal schema definition:**

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "type": "object",
  "required": ["schema", "ts", "device", "stream", "frame_w", "frame_h", "people", "fps", "tracks"],
  "properties": {
    "schema": {
      "type": "string",
      "pattern": "^analytics/v\\d+\\.\\d+$",
      "description": "Schema version identifier"
    },
    "ts": { "type": "number", "minimum": 0 },
    "device": { "type": "string" },
    "stream": { "type": "string" },
    "frame_w": { "type": "integer", "minimum": 1 },
    "frame_h": { "type": "integer", "minimum": 1 },
    "model": { "type": "string" },
    "fw_version": { "type": "string" },
    "npu_load": { "type": "number", "minimum": 0, "maximum": 100 },
    "people": { "type": "integer", "minimum": 0 },
    "people_confident": { "type": "integer", "minimum": 0 },
    "gaze": { "type": "integer", "minimum": 0 },
    "gaze_conf": { "type": "number", "minimum": 0, "maximum": 1 },
    "fps": { "type": "integer", "minimum": 0 },
    "roi": {
      "type": "object",
      "properties": {
        "type": { "type": "string", "enum": ["border", "polygon"] },
        "border_frac": { "type": "number", "minimum": 0, "maximum": 0.5 },
        "rect": { "type": "array", "items": { "type": "number" }, "minItems": 4, "maxItems": 4 }
      }
    },
    "health": {
      "type": "object",
      "properties": {
        "detector_fps": { "type": "number" },
        "tracker_fps": { "type": "number" },
        "queue_latency_ms": { "type": "number" },
        "dropped_frames": { "type": "integer" },
        "last_model_reload_ts": { "type": "number" }
      }
    },
    "tracks": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["id", "state", "bbox", "score", "dir", "deg", "dir_conf", "speed", "speed_norm", "dwell", "enter", "exit"],
        "properties": {
          "id": { "type": "integer", "minimum": 1 },
          "state": { "type": "string", "enum": ["Tentative", "Confirmed", "Lost"] },
          "bbox": { 
            "type": "array", 
            "items": { "type": "number" }, 
            "minItems": 4, 
            "maxItems": 4,
            "description": "[x0, y0, x1, y1] - top-left, bottom-right"
          },
          "score": { "type": "number", "minimum": 0, "maximum": 1 },
          "zones": { "type": "array", "items": { "type": "string" } },
          "dir": { 
            "type": "string", 
            "enum": ["R", "UR", "U", "UL", "L", "DL", "D", "DR", "?"] 
          },
          "deg": { "type": "number", "minimum": 0, "maximum": 360 },
          "dir_conf": { "type": "number", "minimum": 0, "maximum": 1 },
          "speed": { "type": "number", "minimum": 0 },
          "speed_norm": { "type": "number", "minimum": 0, "maximum": 1 },
          "dwell": { "type": "number", "minimum": 0 },
          "enter": { "type": "boolean" },
          "exit": { "type": "boolean" },
          "gaze": {
            "type": "object",
            "description": "Per-person gaze tracking data (optional, only present when face detected)",
            "properties": {
              "detected": { 
                "type": "integer", 
                "enum": [0, 1],
                "description": "Whether person is looking at camera (1=yes, 0=no)"
              },
              "time": { 
                "type": "number", 
                "minimum": 0,
                "description": "Accumulated gaze time in seconds (cumulative across track lifetime)"
              },
              "face_bbox": {
                "type": "array",
                "items": { "type": "number" },
                "minItems": 4,
                "maxItems": 4,
                "description": "Face bounding box [x0, y0, x1, y1] in camera coordinates"
              }
            },
            "required": ["detected", "time", "face_bbox"]
          }
        }
      }
    }
  }
}
```

### Numeric Precision & Coordinate Policy

**Floating-Point Precision:**

- Most numeric fields use **one decimal place** (e.g., `81.5`, `0.87`, `280.16`)
- Exceptions: `ts` (2 decimals), `latency_ms` (0-1 decimals)
- Coordinates and speeds: 1 decimal precision

**Coordinate System:**

- **Bbox coordinates** (`bbox: [x0, y0, x1, y1]`) are **pixel-space floats**
- **Origin:** Top-left corner (0, 0)
- __Bounds:__ Relative to `frame_w` and `frame_h`
   - Valid x range: [0.0, frame_w)
   - Valid y range: [0.0, frame_h)

- **Ordering:** `[x0, y0, x1, y1]` where (x0, y0) = top-left, (x1, y1) = bottom-right
- **Sub-pixel accuracy:** Coordinates may have fractional values (e.g., `21.2`)

**Normalized Values:**

- All fractions (e.g., `score`, `dir_conf`, `speed_norm`) are in range [0.0, 1.0]
- Percentages (e.g., `npu_load`) are in range [0.0, 100.0]

**Integer Fields:**

- `people`, `people_confident`, `gaze`, `fps`, `frame_w`, `frame_h`, `dropped_frames` are integers (no decimals)

## Top-Level Fields

---

## Schema Version & Breaking Changes

### `schema` (string)

**Schema version identifier**

- **Format:** `"analytics/v{major}.{minor}"`
- **Current:** `"analytics/v7.0"`
- **Purpose:** Allows consumers to detect schema changes and handle compatibility

### Client Compatibility & Fallback Behavior

**Handling Unknown Versions:**

- **Major version change (v7 → v8):** Breaking changes likely
   - **Recommended:** Fail fast with clear error message
   - **Alternative:** Parse best-effort, log warnings for unknown fields

- **Minor version change (v7.0 → v7.1):** Additive changes only (new fields, backward compatible)
   - **Recommended:** Ignore unknown fields, continue processing

**Topic-Based Versioning (Optional):**

- System publishes to: `bs/argus/analytics` (version-agnostic topic)
- For consumers preferring topic-based routing:
   - **Option 1:** Subscribe to `bs/argus/analytics/v7` (version-specific)
   - **Option 2:** Subscribe to `bs/argus/analytics/#` (all versions, filter by `schema` field)

- **Note:** Current implementation publishes to single topic; topic versioning is optional future extension

**Example Version Check:**

```javascript
const data = JSON.parse(message);
const [_, major, minor] = data.schema.match(/v(\d+)\.(\d+)/);

if (parseInt(major) > 7) {
  console.error(`Unsupported schema version: ${data.schema}`);
  return; // Fail fast
}
// Process message...
```

### Version History & Breaking Changes

**v7.0 (Current) - ByteTrack Era**

- [x] **BREAKING:** Changed from IoU-based to ByteTrack tracking (IDs more stable)
- [x] **BREAKING:** `speed` now from Kalman Filter velocity (not bbox deltas)
- [x] **NEW:** Added `schema` field for versioning
- [x] **NEW:** Added `state` field to tracks (`Tentative`/`Confirmed`/`Lost`)
- [x] **NEW:** Added `zones` array to tracks
- [x] __NEW:__ Added `frame_w`, `frame_h`, `model`, `fw_version`, `npu_load`
- [x] __NEW:__ Added `people_confident` (score ≥ 0.70)
- [x] __NEW:__ Added `gaze_conf` (gaze detection confidence)
- [x] **NEW:** Added `roi` metadata object
- [x] **NEW:** Added `health` object (system metrics)
- [x] __NEW:__ Added `dir_conf` to tracks
- [x] __NEW:__ Added `speed_norm` to tracks
- [x] **CHANGED:** Publish filtering suppresses low-score/edge tracks (motion forced to 0)

**v6.2 - Direction Confidence**

- Added `dir_conf` field (direction confidence tracking)
- Improved stationary detection
- ROI-based dwell time accumulation

**v6.0 - Initial Release**

- Initial MQTT analytics output
- Multi-track support with persistent IDs
- Entry/exit events
- 8-way direction compass

### `ts` (number)

**Timestamp in seconds**

- **Description:** Monotonic timestamp from system clock (seconds since boot or epoch)
- **Type:** Floating-point number
- **Example:** `955.23` = 955.23 seconds
- **Usage:**
   - Calculate time deltas between messages
   - Synchronize events across multiple streams
   - Measure system latency

### `device` (string)

**Device identifier**

- **Description:** Unique identifier for the hardware device (e.g., BrightSign player serial number)
- **Type:** String
- **Example:** `"XS-156"`
- **Usage:**
   - Multi-device deployments
   - Filter dashboard by specific location/device
   - Device health monitoring

### `stream` (string)

**Video input source**

- **Description:** Camera or video input path
- **Type:** String
- **Examples:**

   - USB camera: `"/dev/video0"`
   - RTSP stream: `"rtsp://192.168.1.100:8554/stream"`
   - Video file: `"/path/to/video.mp4"`

- **Usage:**

   - Identify which camera the data is from
   - Multi-camera setups
   - Debug input source issues

### `people` (integer)

**Current person count**

- **Description:** Total number of confirmed tracks currently being **published** (after all filters applied)
- **Type:** Integer
- **Range:** 0 to N (typically 0-10 in retail/indoor settings)
- **Example:** `2` = two people currently tracked
- **Important:** `people` equals the length of the `tracks` array. Internal tracker state may have more tracks (Tentative, Lost, or filtered), but only Confirmed tracks passing publish filters appear here.
- **Usage:**
   - Real-time occupancy counting
   - Crowd density monitoring
   - Alert when exceeds threshold

- **Note:** Only counts **confirmed tracks** that pass publish filters (score ≥ 0.70, not edge tracks, not tentative)

### `gaze` (integer)

**People gazing at camera**

- **Description:** Number of people currently looking toward the camera/display
- **Type:** Integer
- **Range:** 0 to `people` (always ≤ people count)
- **Example:** `1` = one person is gazing
- **When Present:**
   - Always present in message
   - Set to `0` when gaze detection model is disabled
   - Set to `0` when no one is gazing

- **Usage:**
   - Engagement metrics
   - Attention tracking for digital signage
   - Interactive display triggers

- **Note:** Requires gaze detection model to be enabled for non-zero values (optional feature)

### `fps` (integer)

**Processing frame rate**

- **Description:** Current frames per second being processed
- **Type:** Integer
- **Range:** Typically 15-30 FPS
- **Example:** `29` = processing at 29 FPS
- **Usage:**
   - System performance monitoring
   - Detect processing bottlenecks
   - Alert when FPS drops below threshold

### `frame_w` (integer)

**Frame width**

- **Description:** Camera frame width in pixels
- **Type:** Integer
- **Example:** `640` = 640 pixels wide
- **Usage:**
   - Calculate aspect ratio
   - Convert normalized coordinates
   - Resolution-aware UI scaling
   - Validate bbox coordinates

### `frame_h` (integer)

**Frame height**

- **Description:** Camera frame height in pixels
- **Type:** Integer
- **Example:** `480` = 480 pixels tall
- **Usage:**
   - Calculate frame diagonal for speed normalization
   - Validate bbox coordinates
   - ROI boundary calculations

### `model` (string)

**Detection model name**

- **Description:** Name of the person detection model being used
- **Type:** String
- __Examples:__ `"yolox_s"`, `"yolox_m"`, `"retinaface"`
- **Usage:**
   - Model performance tracking
   - Troubleshooting accuracy issues
   - A/B testing different models

### `fw_version` (string)

**Firmware version**

- **Description:** Software/firmware version of the analytics system
- **Type:** String
- **Example:** `"7.0.0"`
- **Usage:**
   - Compatibility checking
   - Bug tracking and support
   - Feature availability detection

### `npu_load` (number)

**NPU utilization percentage**

- **Description:** Current Neural Processing Unit load (0-100%)
- **Type:** Floating-point number
- **Range:** 0.0 to 100.0
- **Example:** `45.2` = 45.2% NPU utilization
- **Averaging:** Exponential moving average (EMA) over last ~1 second
- **Update Rate:** Smoothed to avoid reacting to instantaneous spikes
- **Usage:**
   - Performance monitoring
   - Thermal management
   - Load balancing decisions
   - Alert on sustained overload (> 90% for 10+ seconds)

- **Note:** Use windowed averages on dashboards to avoid false alarms from transient spikes

### `people_confident` (integer)

**High-confidence person count**

- **Description:** Number of tracks with score ≥ 0.70 (after publish filtering)
- **Type:** Integer
- **Range:** 0 to `people`
- **Example:** `1` = one high-confidence track (vs. 2 total)
- **Usage:**
   - Quality-filtered occupancy counting
   - Separate reliable tracks from edge cases
   - Dashboard "verified count" display

- __Calculation:__ `people_confident = count(tracks where score >= publish_score_min)`

### `gaze_conf` (number)

**Gaze detection confidence**

- **Description:** Average confidence of gaze detections across all gazing people
- **Type:** Floating-point number
- **Range:** 0.0 to 1.0
- **Example:** `0.85` = 85% average gaze confidence
- **When Present:**
   - Always present in message if gaze model is enabled
   - Set to `0.0` when `gaze = 0` (no one gazing)
   - Omitted entirely when gaze detection model is disabled

- **Usage:**
   - Filter uncertain gaze events
   - Engagement quality metrics
   - Weight gaze analytics by confidence

- **Note:** Only present if gaze detection is enabled. Downstream consumers should check for field presence before use.

### `roi` (object)

**Region of Interest metadata**

- **Description:** Configuration of the ROI used for entry/exit and dwell tracking
- **Type:** Object
- **Properties:**
   - `type` (string): `"border"` or `"polygon"`
   - `border_frac` (number): Border fraction for inset ROI (e.g., 0.10 = 10%)
   - `rect` (array): Computed ROI rectangle `[x0, y0, x1, y1]` in pixels

- **Example:**

```json
"roi": {
  "type": "border",
  "border_frac": 0.10,
  "rect": [64, 48, 576, 432]
}
```

- **Usage:**
   - Visualize ROI boundary on dashboard
   - Validate zone membership calculations
   - Debug entry/exit event timing

### `health` (object)

**System health metrics**

- **Description:** Performance and diagnostic metrics for operational monitoring
- **Type:** Object (optional)
- **Properties:**
   - `detector_fps` (number): Detection model inference rate (EMA over ~1s window)
   - `tracker_fps` (number): Tracker update rate (EMA over ~1s window)
   - `queue_latency_ms` (number): Processing queue latency in milliseconds (instantaneous)
   - `dropped_frames` (integer): Frames dropped since last publish (accumulator)
   - `last_model_reload_ts` (number): Timestamp of last model reload

- **Averaging Windows:**
   - `detector_fps` and `tracker_fps` use exponential moving average (EMA) over last ~1 second
   - This smooths transient spikes; dashboards should still use 10-30s windows for alerting
   - `queue_latency_ms` is instantaneous (current value), can spike briefly
   - `dropped_frames` resets each publish cycle (typically every 1-30 seconds)

- **Example:**

```json
"health": {
  "detector_fps": 29.5,
  "tracker_fps": 29.8,
  "queue_latency_ms": 12,
  "dropped_frames": 0,
  "last_model_reload_ts": 850.0
}
```

- **Usage:**
   - Operations dashboard (monitor fleet health)
   - Performance degradation alerts
   - Capacity planning
   - Root cause analysis for tracking issues

- **Alerts:**
   - `detector_fps < 15` → Model overload
   - `queue_latency_ms > 100` → Processing bottleneck
   - `dropped_frames > 0` → System under stress

---

## Track Object Fields

Each object in the `tracks` array represents one tracked person:

### `id` (integer)

**Unique track identifier**

- **Description:** Persistent ID assigned to this person for the duration of their presence
- **Type:** Integer
- **Range:** 1 to N (increments globally, never reused in same session)
- **Example:** `4` = fourth person tracked since system start
- **Usage:**

   - Track individual movement paths over time
   - Calculate per-person dwell time
   - Count unique visitors

- **Persistence:** ID remains stable as person moves around frame, even through brief occlusions

### `state` (string)

**Track lifecycle state**

- **Description:** Current state in the tracking lifecycle
- **Type:** String (enum)
- **Values:**
   - `"Tentative"` = New detection, not yet confirmed (1-3 frames)
   - `"Confirmed"` = Stable track, passes confidence threshold
   - `"Lost"` = Track lost (occlusion or exit), about to be deleted

- **Example:** `"Confirmed"` = track is stable and reliable
- **Published Output:** In MQTT messages, `state` will **always be `"Confirmed"`** because only Confirmed tracks are published. The enum includes all three values for schema completeness, but Tentative and Lost tracks are filtered before publishing.
- **Usage:**
   - Filter out tentative tracks for cleaner analytics (already done by publisher)
   - Detect track churn (rapid Tentative→Lost cycles) — requires internal logs
   - Lifecycle event handling (only count Confirmed→Lost as exits)

- **Lifecycle Flow:**

```ini
Tentative (1-3 frames) → Confirmed (N frames) → Lost (1-3 frames) → Deleted
         ↓                         ↓                     ↓
    (not published)           (published)          (not published, then removed)
```

- **ID Assignment:** IDs assigned in Tentative state, persist through Confirmed, never reused after deletion

### `zones` (array of strings)

**Zone membership**

- **Description:** List of named zones this track's bbox center currently intersects
- **Type:** Array of strings
- **Example:** `["main", "roi"]` = track is in both "main" and "roi" zones
- **Common Zones:**
   - `"roi"` = Inside the main region of interest
   - `"edge"` = Near frame border (within publish_border_frac)
   - `"main"` = Primary monitoring area
   - `"promo"` = Promotional display zone
   - `"checkout"` = Checkout/exit zone
   - Custom zones defined in configuration

- **Usage:**
   - Zone-specific analytics (dwell per zone, zone transitions)
   - Heat maps by zone
   - Trigger zone-specific actions
   - Filter tracks by location

- **Calculation:** Evaluated per frame based on bbox center `(cx, cy)` against configured zone polygons/rectangles
- **Example Workflow:**

```javascript
// Count people in checkout zone
const checkoutCount = data.tracks.filter(t => 
  t.zones.includes('checkout') && t.state === 'Confirmed'
).length;
```

### `bbox` (array of 4 numbers)

**Bounding box coordinates**

- **Description:** Person's location in the frame `[x0, y0, x1, y1]` (top-left and bottom-right corners)
- **Type:** Array of 4 floating-point numbers
- **Units:** Pixels in camera resolution (e.g., 640×480, 1280×720)
- **Example:** `[583.5, 23.7, 638.2, 469.0]`

   - Top-left: (583.5, 23.7)
   - Bottom-right: (638.2, 469.0)
   - Width: 54.7 px, Height: 445.3 px

- **Usage:**

   - Draw bounding boxes on video overlay
   - Calculate person size (distance estimation)
   - Heatmap generation (where people stand)
   - Zone detection (is person in specific area?)

- **Coordinate System:** Origin (0, 0) at top-left corner, x increases right, y increases down

**Calculate derived metrics:**

```javascript
const [x0, y0, x1, y1] = bbox;
const width = x1 - x0;
const height = y1 - y0;
const center_x = (x0 + x1) / 2;
const center_y = (y0 + y1) / 2;
const area = width * height;
```

### `score` (number)

**Detection confidence**

- **Description:** Detector's confidence that this is a real person (0.0 to 1.0)
- **Type:** Floating-point number
- **Range:** 0.0 to 1.0
- **Example:** `0.93` = 93% confidence
- **Interpretation:**

   - **≥ 0.90:** High confidence (well-lit, center frame, full body visible)
   - **0.70 - 0.89:** Medium confidence (partial occlusion, edge of frame)
   - **< 0.70:** Low confidence (filtered out, motion suppressed)

- **Usage:**

   - Quality filtering (only trust high-score tracks)
   - Debug false detections
   - Lighting quality indicator

- **Note:** Tracks with score < 0.70 have their motion suppressed

### `dir` (string)

**Movement direction label**

- **Description:** 8-way compass direction or "?" for stationary/unknown
- **Type:** String (enum)
- **Values:**

   - `"R"` = Right (0°)
   - `"UR"` = Up-Right (45°)
   - `"U"` = Up (90°)
   - `"UL"` = Up-Left (135°)
   - `"L"` = Left (180°)
   - `"DL"` = Down-Left (225°)
   - `"D"` = Down (270°)
   - `"DR"` = Down-Right (315°)
   - `"?"` = Stationary or unknown direction

- **Example:** `"U"` = person walking upward (away from camera)
- **Usage:**

   - Traffic flow analysis
   - Entry/exit counting
   - Path prediction
   - Movement heatmaps

- __Note:__ Only set when `speed ≥ min_speed_px_s` (default 36 px/s). Below this threshold → `"?"`

### `deg` (number)

**Direction in degrees**

- **Description:** Precise movement direction in degrees (0° = right, 90° = up)
- **Type:** Floating-point number
- **Range:** [0.0, 360.0) — values are in range [0, 360), 360 is never emitted
- **Precision:** One decimal place (e.g., `81.5`)
- **Example:** `81.5` = moving almost straight up, slightly right
- **Stationary:** When `dir = "?"` (stationary or below speed threshold), `deg = 0.0`
- **Coordinate System:**

   - 0° = Right (+X direction)
   - 90° = Up (-Y direction)
   - 180° = Left (-X direction)
   - 270° = Down (+Y direction)

- **Usage:**

   - Precise trajectory analysis
   - Calculate angle differences
   - Advanced pathfinding algorithms

- **Note:** Values wrap at 360° (e.g., 359.5° + 1° = 0.5°). Always `0.0` when stationary.

### `dir_conf` (number)

**Direction confidence**

- **Description:** Confidence in the reported direction (0.0 to 1.0)
- **Type:** Floating-point number
- **Range:** 0.0 to 1.0
- **Example:** `0.91` = 91% confident in the reported direction
- **Calculation:** Based on speed margin above threshold and distance from 8-way bin edges

   - High when: fast speed + direction clearly in bin center
   - Low when: slow speed + direction near bin boundary (e.g., 42° between R and UR)

- **Usage:**

   - Filter uncertain directions
   - Weight direction data by confidence
   - Detect indecisive movement (hovering, turning)

- **Interpretation:**

   - **≥ 0.80:** High confidence (clear, sustained motion)
   - **0.50 - 0.79:** Medium confidence (slow or changing direction)
   - **< 0.50:** Low confidence (barely above speed threshold)
   - **0.0:** Stationary or unknown

### `speed` (number)

**Movement speed in pixels/second**

- **Description:** Current speed derived from Kalman Filter velocity estimate
- **Type:** Floating-point number
- **Units:** Pixels per second (px/s)
- __Range:__ 0.0 to ~120.0 (clamped to max_speed_px_s)
- **Example:** `70.8` = moving at 70.8 px/s
- **Interpretation (640×480 @ 30 FPS):**

   - **0 px/s:** Stationary
   - **30-60 px/s:** Slow walking (~1-2 m/s)
   - **60-90 px/s:** Normal walking (~2-3 m/s)
   - **90-120 px/s:** Fast walking/jogging (~3-4 m/s)
   - **> 120 px/s:** Clamped (prevents spikes from edge tracks)

- **Usage:**

   - Detect running vs. walking
   - Measure traffic flow rate
   - Identify loitering (low speed + high dwell)
   - Alert on unusual speeds

- **Scaling:** Speed scales with resolution (1280×720 → multiply by ~1.5×)
- **Note:** Set to `0.0` when below motion threshold or for low-confidence tracks

### `speed_norm` (number)

**Normalized speed**

- **Description:** Speed normalized to frame diagonal (0.0 to 1.0)
- **Type:** Floating-point number
- **Range:** 0.0 to ~1.0
- **Example:** `0.111` = 11.1% of frame diagonal per second
- __Calculation:__ `speed_norm = speed / frame_diagonal`

   - For 640×480: `diagonal = sqrt(640² + 480²) = 800 px`
   - Example: `70.8 / 800 = 0.0885`

- **Usage:**

   - Compare speeds across different camera resolutions
   - Resolution-independent metrics
   - Percentage-based thresholds

- **Interpretation:**

   - **< 0.05:** Stationary or very slow
   - **0.05 - 0.15:** Normal walking
   - **> 0.15:** Fast walking/running

### `dwell` (number)

**Dwell time in seconds**

- **Description:** Total time (seconds) this person has spent **inside the region of interest (ROI)** while confirmed
- **Type:** Floating-point number
- **Units:** Seconds
- **Range:** 0.0 to N (increases monotonically while in ROI)
- **Example:** `280.16` = 280.16 seconds = 4 minutes 40 seconds
- **Accumulation Rules:**

   - Only increments when **confirmed** (not tentative)
   - Only increments when **inside ROI** (default: 10% inset from frame border)
   - Pauses when person exits ROI
   - Resets to 0 when track is deleted

- **Usage:**

   - Measure engagement time
   - Identify long-term viewers vs. passers-by
   - Calculate average dwell per visitor
   - Trigger actions after dwell threshold (e.g., "customer service needed after 60s")

- __ROI Definition:__ Configurable via `enter_exit_border_frac` (default 0.10 = 10% border)

   - For 640×480: ROI is approximately [64, 48] to [576, 432]

### `enter` (boolean)

**Entry event flag**

- **Description:** One-shot flag indicating person just entered the ROI **this frame**
- **Type:** Boolean
- **Example:** `true` = person crossed into ROI boundary this frame
- **Behavior:**

   - `true` for **exactly one frame** when person enters ROI
   - `false` all other times (including while inside ROI)
   - Resets to `false` after being read/published
   - **Cannot be true simultaneously with `exit`** (mutually exclusive per frame per track)
   - **Can repeat** if person exits and re-enters ROI (new entry event triggered)

- **Usage:**

   - Count entries (increment counter when `enter = true`)
   - Trigger entry notifications/events
   - Log entry timestamps
   - Calculate traffic patterns

- **Typical Pattern:**

```ini
Frame 1: enter=false (outside ROI)
Frame 2: enter=false (outside ROI)
Frame 3: enter=true  (crossed boundary) ← Trigger event
Frame 4: enter=false (inside ROI, event consumed)
Frame 5: enter=false (inside ROI)
...
```

### `exit` (boolean)

**Exit event flag**

- **Description:** One-shot flag indicating person just exited the ROI **this frame**
- **Type:** Boolean
- **Example:** `true` = person crossed out of ROI boundary this frame
- **Behavior:**

   - `true` for **exactly one frame** when person exits ROI
   - `false` all other times (including while outside ROI)
   - Resets to `false` after being read/published
   - **Cannot be true simultaneously with `enter`** (mutually exclusive per frame per track)
   - **Can repeat** if person re-enters and exits ROI again (new exit event triggered)

- **Usage:**

   - Count exits (increment counter when `exit = true`)
   - Trigger exit notifications/events
   - Calculate pass-through vs. dwell time
   - Pair with `enter` for visit duration

- **Example Workflow:**

```javascript
// Count unique visits
if (track.enter) {
  visitStart[track.id] = ts;
  entriesCount++;
}
if (track.exit) {
  const visitDuration = ts - visitStart[track.id];
  exitsCount++;
  totalDwell += visitDuration;
}
```

### `gaze` (object, optional)

**Per-person gaze tracking data**

- **Description:** Per-person gaze detection data linking face detection to person tracks. **Only present when a face is detected and successfully matched to this person track.**
- **Type:** Object with three fields: `detected`, `time`, and `face_bbox`
- **Conditional Presence:**
  - **Present:** When RetinaFace detects a face AND the face is matched to this person using spatial IoU matching
  - **Absent:** When no face detected for this person, or face detected but not matched
  - In production, typically 10-20% of person tracks have gaze data at any given moment
  
- **Matching Algorithm:** 
  - Face bounding boxes are expanded 30% upward to bridge spatial gap between head detection (YOLOX) and face detection (RetinaFace)
  - IoU (Intersection over Union) threshold of 0.1 used for matching
  - Each face matched to at most one person track per frame

- **Example (from production 1280×720 RTSP stream):**
  ```json
  "gaze": {
    "detected": 1,
    "time": 8.00,
    "face_bbox": [231, 167, 240, 180]
  }
  ```
  
- **Usage:**
  - Identify which specific people are looking at the camera/display
  - Measure per-person attention duration
  - Correlate gaze with demographics, movement patterns, dwell time
  - Generate engagement heatmaps by person location

- **Limitations:**
  - 320×320 RetinaFace model may struggle with distant faces in high-resolution streams
  - Face detection rate depends on lighting, angle, distance, and occlusion
  - Not all person tracks will have gaze data simultaneously

#### `gaze.detected` (number)

**Current gaze state (0 = looking away, 1 = gazing at camera)**

- **Description:** Binary indicator of whether this person is currently looking at the camera/display
- **Type:** Integer (0 or 1)
- **Values:**
  - `0` = Person's face is visible but they are **looking away** (not making eye contact with camera)
  - `1` = Person is **gazing at the camera/display** (making eye contact)
  
- **Example:** `"detected": 1` = person is currently looking at the camera

- **Persistence:**
  - Value updates every frame based on real-time gaze classification
  - Can change from 0 ↔ 1 as person looks toward/away from camera
  - Used to gate the accumulation of `gaze.time`

- **Classification:** 
  - Based on head pose estimation and facial landmark analysis
  - Threshold optimized for retail/signage viewing angles
  
- **Usage:**
  - Real-time attention alerts ("Customer 52 is looking at display NOW")
  - Live engagement dashboards showing who is currently engaged
  - Trigger content changes when gaze detected
  - Filter analytics to only count attentive viewers

#### `gaze.time` (number)

**Cumulative gaze duration in seconds**

- **Description:** Total time (seconds) this person has spent **gazing at the camera/display** across the entire lifetime of their track
- **Type:** Floating-point number
- **Units:** Seconds
- **Range:** 0.0 to N (increases monotonically when `detected = 1`)
- **Example:** `"time": 8.00` = person has looked at camera for 8.00 seconds total

- **Accumulation Rules:**
  - Only increments when `gaze.detected = 1` (actively gazing)
  - Pauses when `gaze.detected = 0` (looking away) but does **not reset**
  - Persists across the entire track lifetime
  - Increments at frame rate (e.g., +0.033s per frame at 30 FPS when gazing)
  - Resets to 0 only when track is deleted/lost

- **Usage:**
  - Measure total attention/engagement per person
  - Rank people by attention duration ("Top 10 most engaged viewers")
  - Calculate attention metrics: `attention_rate = gaze.time / dwell`
  - Identify highly engaged vs. distracted viewers
  - Trigger actions after attention threshold (e.g., "Show special offer after 5s gaze")

- **Interpretation:**
  - **< 1.0s:** Brief glance
  - **1-5s:** Short engagement
  - **5-15s:** Moderate engagement
  - **> 15s:** High engagement / interested viewer

- **Production Example (1280×720 RTSP, 8 people):**
  - Track 63: `gaze.time = 8.00` (8 seconds of accumulated gaze)
  - Track 79, 50: No gaze object (faces not detected)
  - This demonstrates typical conditional presence: 1 of 8 people with gaze data

#### `gaze.face_bbox` (array[4])

**Face bounding box coordinates [x0, y0, x1, y1]**

- **Description:** Pixel coordinates of the detected face in the camera frame
- **Type:** Array of 4 integers
- **Format:** `[x0, y0, x1, y1]` where (x0, y0) is top-left corner, (x1, y1) is bottom-right corner
- **Coordinate System:** Same as track `bbox` (camera pixel coordinates)
- **Example:** `"face_bbox": [231, 167, 240, 180]`
  - Top-left: (231, 167)
  - Bottom-right: (240, 180)
  - Face width: 9 pixels
  - Face height: 13 pixels

- **Usage:**
  - Draw face bounding boxes in debug/visualization mode
  - Calculate face size for quality assessment
  - Verify spatial alignment between person `bbox` and `face_bbox`
  - Detect faces that are too small/distant for reliable gaze classification

- **Spatial Relationship:**
  - Face bbox typically appears in **upper portion** of person bbox
  - In production, face Y-coordinates often 100-200px above person bbox Y-coordinates
  - This spatial gap is bridged by 30% upward expansion during matching

- **Size Guidelines:**
  - **< 10×10 px:** Very small face, gaze classification may be unreliable
  - **10-30 px:** Small face, typical for distant people in high-res streams
  - **30-100 px:** Medium face, good gaze classification quality
  - **> 100 px:** Large face, excellent gaze classification quality

- **Production Example (1280×720 RTSP):**
  ```json
  "gaze": {
    "detected": 1,
    "time": 8.00,
    "face_bbox": [231, 167, 240, 180]  // 9×13 pixel face
  }
  ```
  - Small face size (9×13 px) indicates person is relatively distant from camera
  - Despite small size, gaze was successfully classified as detected=1

**Handling Optional Gaze Data (JavaScript Example):**

```javascript
// Process tracks with optional gaze data
data.tracks.forEach(track => {
  console.log(`Track ${track.id}: dwell=${track.dwell.toFixed(1)}s`);
  
  // Check if gaze data is available for this person
  if (track.gaze) {
    const isGazing = track.gaze.detected === 1;
    const gazeTime = track.gaze.time;
    const attentionRate = (gazeTime / track.dwell * 100).toFixed(1);
    
    console.log(`  Gaze: ${isGazing ? 'LOOKING' : 'away'}, ` +
                `time=${gazeTime.toFixed(1)}s, ` +
                `attention=${attentionRate}%`);
    
    // Trigger action if person gazing for > 5 seconds
    if (gazeTime > 5.0 && isGazing) {
      displaySpecialOffer(track.id);
    }
  } else {
    console.log('  Gaze: no face detected');
  }
});
```

**Real-World Example (8 people in 1280×720 RTSP stream):**

```json
{
  "tracks": [
    {
      "id": 63,
      "bbox": [622, 285, 654, 370],
      "dwell": 12.5,
      "gaze": {
        "detected": 1,
        "time": 8.00,
        "face_bbox": [231, 167, 240, 180]
      }
    },
    {
      "id": 79,
      "bbox": [890, 310, 925, 410],
      "dwell": 5.2
      // No gaze object - face not detected for this person
    },
    {
      "id": 50,
      "bbox": [450, 295, 485, 385],
      "dwell": 18.7
      // No gaze object - face detected but not matched to this track
    }
  ]
}
```

**Interpretation:**
- Track 63: **Has gaze data** - face detected, matched, currently gazing (detected=1), 8s total gaze time, 64% attention rate (8.0/12.5)
- Track 79: **No gaze data** - person detected but face not visible/detected (turned away, too far, occluded)
- Track 50: **No gaze data** - face may have been detected but failed IoU matching threshold with this track's bbox

This is typical production behavior: in a scene with 8 people, only 1-3 will have gaze data at any moment. Dashboard code must handle both cases gracefully with null checks.

---

## Motion Detection & Speed Thresholds

### Default Configuration Values

| Parameter | Default | Unit | Description |
|-----------|---------|------|-------------|
| `min_speed_px_s` | 36.0 | px/s | Minimum speed to show direction (stationary below this) |
| `max_speed_px_s` | 120.0 | px/s | Maximum speed clamp (prevents spikes) |
| `moving_streak_req` | 3 | frames | Consecutive frames above min_speed to set direction |
| `publish_score_min` | 0.70 | 0-1 | Minimum score for motion publishing |
| `publish_min_area_frac` | 0.02 | 0-1 | Minimum bbox area (fraction of frame) |
| `publish_border_frac` | 0.03 | 0-1 | Border exclusion zone (fraction from edge) |
| `enter_exit_border_frac` | 0.10 | 0-1 | ROI inset border for entry/exit |
| `kf_ema_alpha` | 0.20 | 0-1 | EMA smoothing for Kalman velocity (optional) |

### Speed Interpretation (640×480 resolution)

| Speed Range (px/s) | Real-World Speed | Interpretation |
|-------------------|------------------|----------------|
| 0 | 0 m/s | Stationary (below threshold or filtered) |
| 1-35 | 0-1 m/s | Below motion floor (shows as stationary) |
| 36-60 | 1-2 m/s | Slow walking |
| 60-90 | 2-3 m/s | Normal walking |
| 90-120 | 3-4 m/s | Fast walking / jogging |
| > 120 | N/A | Clamped (prevents edge track spikes) |

**Note:** Speed scales linearly with resolution. For 1280×720, multiply by ~1.5×.

### Direction Bins (8-Way Compass)

| Direction | Degree Range | Bin Center | Enum |
|-----------|-------------|------------|------|
| Right | 337.5° - 22.5° | 0° | `"R"` |
| Up-Right | 22.5° - 67.5° | 45° | `"UR"` |
| Up | 67.5° - 112.5° | 90° | `"U"` |
| Up-Left | 112.5° - 157.5° | 135° | `"UL"` |
| Left | 157.5° - 202.5° | 180° | `"L"` |
| Down-Left | 202.5° - 247.5° | 225° | `"DL"` |
| Down | 247.5° - 292.5° | 270° | `"D"` |
| Down-Right | 292.5° - 337.5° | 315° | `"DR"` |

### Hysteresis & Debouncing

- __Moving Streak:__ Requires `moving_streak_req` consecutive frames above `min_speed_px_s` before setting direction

   - **Purpose:** Prevents jitter from detector noise
   - **Example:** 3 consecutive frames at ≥36 px/s before `dir != "?"`

- **Direction Confidence:** Increases with distance from bin edges

   - **High conf:** Speed well above threshold + direction in bin center
   - **Low conf:** Speed near threshold + direction near bin boundary (e.g., 42° between R and UR)

---

## Publish Filtering Behavior

**Important:** Not all detected tracks are published with motion. The system applies multi-layer filtering to suppress noise from edge/ghost tracks:

### Motion Suppression Rules

A track will show `speed: 0.0, dir: "?"` (stationary) if **any** of these conditions are true:

1. **Low Confidence:** `score < 0.70`
2. **Edge Track:** Bbox center within 3% of frame border
3. **Small Area:** Bbox area < 2% of frame
4. **Stale:** Not updated this frame (`missed > 0`)
5. __Below Speed Floor:__ Speed < `min_speed_px_s` (default 36 px/s)
6. **Motion Streak:** Less than 3 consecutive frames above speed floor

### Example Scenarios

**Scenario 1: Clean Central Track**

```json
{
  "id": 2,
  "bbox": [21.2, 143.7, 421.3, 475.1],  // Center of frame
  "score": 0.93,                          // High confidence [x]
  "dir": "?",
  "speed": 0.0,                           // Stationary (below speed floor)
  "dwell": 280.16                         // Accumulating dwell time
}
```

- **Interpretation:** High-quality track, stationary (person standing still)

**Scenario 2: Edge Track (Suppressed)**

```json
{
  "id": 4,
  "bbox": [583.5, 23.7, 638.2, 469.0],   // Near right edge (x=583/640)
  "score": 0.61,                          // Low confidence [ ]
  "dir": "?",
  "speed": 0.0,                           // Motion suppressed
  "dwell": 0.00                           // Not accumulating (filtered)
}
```

- **Interpretation:** Edge track with low confidence → motion suppressed even if KF has velocity

**Scenario 3: Moving Track**

```json
{
  "id": 5,
  "bbox": [200.0, 150.0, 300.0, 450.0],
  "score": 0.88,                          // High confidence [x]
  "dir": "L",
  "deg": 175.0,
  "dir_conf": 0.85,
  "speed": 55.3,                          // Moving [x]
  "speed_norm": 0.087,
  "dwell": 12.5
}
```

- **Interpretation:** Person walking left at normal speed

---

## Complete Message Examples

### Example 1: Empty Frame (No People)

**No detections, clean system state:**

```json
{
  "schema": "analytics/v7.0",
  "ts": 120.45,
  "device": "XS-156",
  "stream": "/dev/video0",
  "frame_w": 640,
  "frame_h": 480,
  "model": "yolox_s",
  "fw_version": "7.0.0",
  "npu_load": 25.3,
  "people": 0,
  "people_confident": 0,
  "gaze": 0,
  "gaze_conf": 0.0,
  "fps": 30,
  "roi": {
    "type": "border",
    "border_frac": 0.10,
    "rect": [64, 48, 576, 432]
  },
  "health": {
    "detector_fps": 30.1,
    "tracker_fps": 30.0,
    "queue_latency_ms": 8,
    "dropped_frames": 0,
    "last_model_reload_ts": 0.0
  },
  "tracks": []
}
```

**Interpretation:** System running normally, no people detected.

---

### Example 2: Single Stationary Person

**One person standing still in center frame:**

```json
{
  "schema": "analytics/v7.0",
  "ts": 450.67,
  "device": "XS-156",
  "stream": "/dev/video0",
  "frame_w": 640,
  "frame_h": 480,
  "model": "yolox_s",
  "fw_version": "7.0.0",
  "npu_load": 38.7,
  "people": 1,
  "people_confident": 1,
  "gaze": 1,
  "gaze_conf": 0.92,
  "fps": 29,
  "roi": {
    "type": "border",
    "border_frac": 0.10,
    "rect": [64, 48, 576, 432]
  },
  "health": {
    "detector_fps": 29.8,
    "tracker_fps": 29.5,
    "queue_latency_ms": 15,
    "dropped_frames": 0,
    "last_model_reload_ts": 0.0
  },
  "tracks": [
    {
      "id": 15,
      "state": "Confirmed",
      "bbox": [180.5, 120.3, 440.2, 475.8],
      "score": 0.95,
      "zones": ["main", "roi"],
      "dir": "?",          // Stationary (speed below threshold)
      "deg": 0.0,
      "dir_conf": 0.00,
      "speed": 0.0,        // Below min_speed_px_s (36 px/s)
      "speed_norm": 0.000,
      "dwell": 85.34,      // Person has been here 85 seconds
      "enter": false,
      "exit": false
    }
  ]
}
```

**Interpretation:** High-quality stationary track, person gazing at camera, accumulating dwell time.

---

### Example 3: Person Walking Left

**One person moving left at normal pace:**

```json
{
  "schema": "analytics/v7.0",
  "ts": 650.12,
  "device": "XS-156",
  "stream": "/dev/video0",
  "frame_w": 640,
  "frame_h": 480,
  "model": "yolox_s",
  "fw_version": "7.0.0",
  "npu_load": 42.1,
  "people": 1,
  "people_confident": 1,
  "gaze": 0,
  "gaze_conf": 0.0,
  "fps": 30,
  "roi": {
    "type": "border",
    "border_frac": 0.10,
    "rect": [64, 48, 576, 432]
  },
  "health": {
    "detector_fps": 30.0,
    "tracker_fps": 29.9,
    "queue_latency_ms": 10,
    "dropped_frames": 0,
    "last_model_reload_ts": 0.0
  },
  "tracks": [
    {
      "id": 22,
      "state": "Confirmed",
      "bbox": [250.0, 140.0, 380.0, 460.0],
      "score": 0.88,
      "zones": ["main", "roi"],
      "dir": "L",          // Moving left
      "deg": 175.5,        // Almost straight left (180° = pure left)
      "dir_conf": 0.87,    // High confidence
      "speed": 65.3,       // Normal walking speed
      "speed_norm": 0.082, // 8.2% of frame diagonal per second
      "dwell": 18.5,       // 18.5 seconds in ROI
      "enter": false,
      "exit": false
    }
  ]
}
```

**Interpretation:** Person walking left at normal speed, confident direction, accumulating dwell time.

---

### Example 4: Multiple People, Mixed States

**Two confirmed people + one edge track (motion suppressed):**

```json
{
  "schema": "analytics/v7.0",
  "ts": 955.23,
  "device": "XS-156",
  "stream": "/dev/video0",
  "frame_w": 640,
  "frame_h": 480,
  "model": "yolox_s",
  "fw_version": "7.0.0",
  "npu_load": 52.4,
  "people": 3,
  "people_confident": 2,  // Only 2 pass score threshold
  "gaze": 1,
  "gaze_conf": 0.85,
  "fps": 29,
  "roi": {
    "type": "border",
    "border_frac": 0.10,
    "rect": [64, 48, 576, 432]
  },
  "health": {
    "detector_fps": 29.5,
    "tracker_fps": 29.2,
    "queue_latency_ms": 18,
    "dropped_frames": 0,
    "last_model_reload_ts": 850.0
  },
  "tracks": [
    {
      "id": 2,
      "state": "Confirmed",
      "bbox": [21.2, 143.7, 421.3, 475.1],
      "score": 0.93,                    // High confidence [x]
      "zones": ["main", "roi"],
      "dir": "?",                        // Stationary
      "deg": 0.0,
      "dir_conf": 0.00,
      "speed": 0.0,
      "speed_norm": 0.000,
      "dwell": 280.16,                  // Long dwell (4min 40s)
      "enter": false,
      "exit": false
    },
    {
      "id": 4,
      "state": "Confirmed",
      "bbox": [583.5, 23.7, 638.2, 469.0],  // Near right edge
      "score": 0.61,                         // Low confidence [ ]
      "zones": ["edge"],                     // In edge zone
      "dir": "?",                            // Motion suppressed
      "deg": 0.0,                            // Despite KF having velocity
      "dir_conf": 0.00,
      "speed": 0.0,                          // Forced to 0 (edge track filter)
      "speed_norm": 0.000,
      "dwell": 0.00,                         // Not accumulating (filtered)
      "enter": false,
      "exit": false
    },
    {
      "id": 8,
      "state": "Confirmed",
      "bbox": [200.0, 150.0, 350.0, 450.0],
      "score": 0.87,                         // High confidence [x]
      "zones": ["main", "roi"],
      "dir": "UR",                           // Moving up-right
      "deg": 52.3,                           // Between up and up-right
      "dir_conf": 0.78,                      // Medium confidence
      "speed": 48.5,                         // Slow-moderate walking
      "speed_norm": 0.061,
      "dwell": 12.8,
      "enter": false,
      "exit": false
    }
  ]
}
```

**Interpretation:**

- **Track 2:** High-quality stationary person, long dwell, gazing at camera
- **Track 4:** Edge track with low score → motion suppressed despite KF velocity
- **Track 8:** Person walking up-right at moderate speed, good quality
- **Summary:** 3 total people, 2 confident (tracks 2 & 8), 1 edge/noise (track 4)

---

### Example 5: Entry Event

**Person just entered ROI (entry flag set):**

```json
{
  "schema": "analytics/v7.0",
  "ts": 1024.56,
  "device": "XS-156",
  "stream": "/dev/video0",
  "frame_w": 640,
  "frame_h": 480,
  "model": "yolox_s",
  "fw_version": "7.0.0",
  "npu_load": 40.2,
  "people": 1,
  "people_confident": 1,
  "gaze": 0,
  "gaze_conf": 0.0,
  "fps": 30,
  "roi": {
    "type": "border",
    "border_frac": 0.10,
    "rect": [64, 48, 576, 432]
  },
  "health": {
    "detector_fps": 30.1,
    "tracker_fps": 30.0,
    "queue_latency_ms": 12,
    "dropped_frames": 0,
    "last_model_reload_ts": 850.0
  },
  "tracks": [
    {
      "id": 35,
      "state": "Confirmed",
      "bbox": [120.0, 50.0, 250.0, 400.0],  // Just crossed ROI boundary
      "score": 0.89,
      "zones": ["roi"],                      // Now in ROI
      "dir": "D",                            // Moving down (into frame)
      "deg": 268.0,
      "dir_conf": 0.92,
      "speed": 72.5,
      "speed_norm": 0.091,
      "dwell": 0.03,                         // Just entered (near zero)
      "enter": true,                         // ← Entry event (one frame only)
      "exit": false
    }
  ]
}
```

**Interpretation:** Person entered ROI this frame. `enter=true` for exactly one frame. Next frame will show `enter=false` with increasing dwell time.

---

## Dashboard Use Cases

### 1. Real-Time Occupancy Counter

```javascript
mqtt.on('message', (topic, message) => {
  const data = JSON.parse(message);
  
  // Simple occupancy
  document.getElementById('count').innerText = data.people;
  
  // With confidence filtering
  const highConfTracks = data.tracks.filter(t => t.score >= 0.80);
  document.getElementById('confident-count').innerText = highConfTracks.length;
});
```

### 2. Traffic Flow Heatmap

```javascript
// Collect bbox centers over time
const heatmapData = [];

mqtt.on('message', (topic, message) => {
  const data = JSON.parse(message);
  
  data.tracks.forEach(track => {
    const [x0, y0, x1, y1] = track.bbox;
    const cx = (x0 + x1) / 2;
    const cy = (y0 + y1) / 2;
    
    heatmapData.push({ x: cx, y: cy, value: 1 });
  });
  
  // Render heatmap with heatmap.js or similar
  updateHeatmap(heatmapData);
});
```

### 3. Dwell Time Analytics

```javascript
const dwellBuckets = {
  '0-10s': 0,
  '10-30s': 0,
  '30-60s': 0,
  '60s+': 0
};

mqtt.on('message', (topic, message) => {
  const data = JSON.parse(message);
  
  data.tracks.forEach(track => {
    if (track.dwell < 10) dwellBuckets['0-10s']++;
    else if (track.dwell < 30) dwellBuckets['10-30s']++;
    else if (track.dwell < 60) dwellBuckets['30-60s']++;
    else dwellBuckets['60s+']++;
  });
  
  updateChart(dwellBuckets);
});
```

### 4. Entry/Exit Counting

```javascript
let entries = 0;
let exits = 0;

mqtt.on('message', (topic, message) => {
  const data = JSON.parse(message);
  
  data.tracks.forEach(track => {
    if (track.enter) {
      entries++;
      console.log(`Person ${track.id} entered at ${data.ts}`);
    }
    if (track.exit) {
      exits++;
      console.log(`Person ${track.id} exited at ${data.ts}`);
    }
  });
  
  document.getElementById('entries').innerText = entries;
  document.getElementById('exits').innerText = exits;
  document.getElementById('current').innerText = entries - exits;
});
```

### 5. Direction Flow Analysis

```javascript
const directionCounts = {
  'R': 0, 'UR': 0, 'U': 0, 'UL': 0,
  'L': 0, 'DL': 0, 'D': 0, 'DR': 0
};

mqtt.on('message', (topic, message) => {
  const data = JSON.parse(message);
  
  data.tracks.forEach(track => {
    if (track.dir !== '?' && track.dir_conf > 0.7) {
      directionCounts[track.dir]++;
    }
  });
  
  // Render compass rose or flow diagram
  updateFlowDiagram(directionCounts);
});
```

### 6. Speed Distribution

```javascript
const speedBuckets = {
  'Stationary': 0,
  'Slow (< 40 px/s)': 0,
  'Normal (40-80 px/s)': 0,
  'Fast (> 80 px/s)': 0
};

mqtt.on('message', (topic, message) => {
  const data = JSON.parse(message);
  
  data.tracks.forEach(track => {
    if (track.speed === 0) speedBuckets['Stationary']++;
    else if (track.speed < 40) speedBuckets['Slow (< 40 px/s)']++;
    else if (track.speed < 80) speedBuckets['Normal (40-80 px/s)']++;
    else speedBuckets['Fast (> 80 px/s)']++;
  });
  
  updateHistogram(speedBuckets);
});
```

### 7. Performance Monitoring

```javascript
const fpsHistory = [];
const maxHistory = 60; // Keep 60 seconds

mqtt.on('message', (topic, message) => {
  const data = JSON.parse(message);
  
  fpsHistory.push({ ts: data.ts, fps: data.fps });
  if (fpsHistory.length > maxHistory) fpsHistory.shift();
  
  // Alert on performance degradation
  if (data.fps < 20) {
    console.warn(`Low FPS: ${data.fps} on device ${data.device}`);
  }
  
  updateFPSChart(fpsHistory);
});
```

---

## Message Frequency

- **Publish Rate:** Configurable, typically 1-30 Hz

   - Default: 1 Hz (1 message per second)
   - Low latency: 10-30 Hz
   - Bandwidth constrained: 0.2-1 Hz

- **Processing Rate:** 15-30 FPS (independent of publish rate)
- **Tracks Array:** Updated every message, empty if no people detected

---

## MQTT Connection Details

### Topic

- **Production:** `bs/argus/analytics`
- __Format:__ `bs/argus/{message_type}`

### QoS (Quality of Service)

- **Recommended:** QoS 0 (At most once)

   - Low latency, no retries
   - Acceptable for real-time analytics (occasional message loss OK)

- **Alternative:** QoS 1 (At least once)

   - Guaranteed delivery, higher overhead
   - Use if exact counts are critical

### Broker

- **Embedded:** System runs local Mosquitto broker
- **Default Port:** 1883 (non-TLS) or 8883 (TLS)
- **Authentication:** Configurable (typically none for local connections)

### Subscription Example

```javascript
const mqtt = require('mqtt');
const client = mqtt.connect('mqtt://localhost:1883');

client.on('connect', () => {
  client.subscribe('bs/argus/analytics', (err) => {
    if (!err) console.log('Subscribed to analytics');
  });
});

client.on('message', (topic, message) => {
  const data = JSON.parse(message.toString());
  // Process data...
});
```

### TLS/Secure Connection Example

**For remote brokers with TLS encryption:**

```javascript
const mqtt = require('mqtt');
const fs = require('fs');

// TLS connection options
const options = {
  port: 8883,
  protocol: 'mqtts',
  rejectUnauthorized: true,  // Verify server certificate
  ca: fs.readFileSync('/path/to/ca.crt'),        // CA certificate
  cert: fs.readFileSync('/path/to/client.crt'),  // Client certificate (optional)
  key: fs.readFileSync('/path/to/client.key'),   // Client key (optional)
  username: 'analytics_user',                    // Authentication
  password: 'secure_password'
};

const client = mqtt.connect('mqtts://broker.example.com:8883', options);

client.on('connect', () => {
  console.log('Secure connection established');
  client.subscribe('bs/argus/analytics', { qos: 1 });
});

client.on('message', (topic, message) => {
  const data = JSON.parse(message.toString());
  // Process data...
});

client.on('error', (err) => {
  console.error('MQTT error:', err);
});
```

### Security Best Practices

**Local Network (Non-Production):**

- QoS 0, no TLS (low latency, acceptable for isolated networks)
- No authentication required for local-only brokers
- Firewall rules to prevent external access

**Remote/Cloud Brokers (Production):**

- **Always use TLS** (port 8883, `mqtts://` protocol)
- **Client certificates** for mutual TLS authentication
- **Username/password** authentication minimum
- **ACL (Access Control Lists)** to restrict topic access
- **Rate limiting** to prevent DoS
- **Regular certificate rotation** (90-day max)

**Data Privacy:**

- Analytics data may contain PII (track positions, dwell times)
- Consider encryption at rest for stored messages
- Comply with GDPR/privacy regulations if applicable
- Option to anonymize or aggregate data before transmission

---

## Performance Considerations

### Message Size

- **Typical:** 200-500 bytes per message (2 tracks)
- **Large:** 1-2 KB (10 tracks)
- **Bandwidth:** ~0.5-2 KB/s @ 1 Hz, ~5-20 KB/s @ 10 Hz

### Latency

- **Processing:** 30-50 ms (detection + tracking)
- **MQTT Publish:** < 5 ms (local broker)
- **Total:** ~35-55 ms (end-to-end)

### Scalability

- **Single Device:** Handles 1-10 tracks easily
- **Multi-Device:** MQTT broker can handle 100+ devices
- **Dashboard:** Use throttling/debouncing for UI updates at high frequency

---

## Troubleshooting

### No Messages Received

1. Check MQTT broker is running: `ps aux | grep mosquitto`
2. Test subscription: `mosquitto_sub -h localhost -t 'bs/argus/#' -v`
3. Check firewall/network connectivity

### Empty `tracks` Array

- No people detected in frame
- All detections below confidence threshold (score < 0.50)
- Check camera view and lighting

### `dir: "?"` for Moving People

- Speed below `min_speed_px_s` threshold (default 36 px/s)
- Low confidence track (score < 0.70) → motion suppressed
- Edge track → motion suppressed
- Increase speed threshold or check tracking quality

### High `id` Numbers

- IDs increment globally, never reused in session
- High numbers indicate many people tracked since system start
- Not a problem, just informational

### Fluctuating `people` Count

- Brief occlusions causing track loss
- Edge detections appearing/disappearing
- Increase `publish_grace_missed` or improve lighting

### Message Validation

**Validating messages against JSON Schema helps catch integration issues early.**

#### JavaScript (Node.js) with Ajv

```javascript
const Ajv = require('ajv');
const mqtt = require('mqtt');

// Load schema from earlier in this document
const schema = {
  "$schema": "http://json-schema.org/draft-07/schema#",
  "type": "object",
  "required": ["schema", "ts", "device", "stream", "frame_w", "frame_h", "people", "fps", "tracks"],
  // ... (full schema from JSON Schema section)
};

const ajv = new Ajv();
const validate = ajv.compile(schema);

const client = mqtt.connect('mqtt://localhost:1883');

client.on('message', (topic, message) => {
  const data = JSON.parse(message.toString());
  
  if (!validate(data)) {
    console.error('Schema validation failed:', validate.errors);
    // Log or handle invalid message
    return;
  }
  
  // Message is valid, process it
  console.log(`Valid message: ${data.people} people detected`);
});
```

#### Python with jsonschema

```python
import json
import paho.mqtt.client as mqtt
from jsonschema import validate, ValidationError

# Load schema from earlier in this document
schema = {
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "required": ["schema", "ts", "device", "stream", "frame_w", "frame_h", "people", "fps", "tracks"],
    # ... (full schema from JSON Schema section)
}

def on_message(client, userdata, msg):
    data = json.loads(msg.payload.decode())
    
    try:
        validate(instance=data, schema=schema)
        print(f"Valid message: {data['people']} people detected")
        # Process valid message
    except ValidationError as e:
        print(f"Schema validation failed: {e.message}")
        # Log or handle invalid message

client = mqtt.Client()
client.on_message = on_message
client.connect("localhost", 1883)
client.subscribe("bs/argus/analytics")
client.loop_forever()
```

---

## Configuration Reference

Key parameters affecting published data (see `configs/argus-config.json`):

```json
{
  "tracker": {
    "tracker_core": "byte",
    "min_speed_px_s": 36.0,
    "publish_score_min": 0.70,
    "publish_min_area_frac": 0.02,
    "publish_border_frac": 0.03,
    "moving_streak_req": 3,
    "enter_exit_border_frac": 0.10
  },
  "publishers": {
    "mqtt": {
      "enabled": true,
      "publish_hz": 1.0
    }
  }
}
```

---

## Related Documentation

- **[MQTT Integration Guide](INTEGRATION-MQTT.md)** - Quick start with code examples
- **[Tracking Explained](TRACKING-EXPLAINED.md)** - Person tracking lifecycle
- **[Configuration Reference](CONFIGURATION.md)** - All configuration options
- **[README](../README.md)** - Project overview

## Support & Contact

For questions, issues, or feature requests:

- **GitHub:** [argus-audience-measurement-extension](https://github.com/brightsign/argus-audience-measurement-extension)
- **Documentation:** `/docs` folder in repository

---

## Changelog

### v7.0 (Current)

- [x] ByteTrack integration for stable IDs
- [x] KF velocity for clean motion detection
- [x] Publish filtering for edge/ghost track suppression
- [x] Motion streak debouncing
- [x] `dir_conf` field added
- [x] `speed_norm` field added

### v6.2

- Direction confidence tracking
- Improved stationary detection
- ROI-based dwell time

### v6.0

- Initial MQTT analytics output
- Multi-track support
- Entry/exit events
