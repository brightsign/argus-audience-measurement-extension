# Face Blurring Technique Evaluation

## Overview

This document evaluates techniques for blurring detected faces in output frames for privacy protection. The blurring must occur **after** all gaze detection and analysis is complete, affecting only the frames written to `/tmp` for visualization purposes.

## Requirements

1. **Non-destructive to pipeline**: Blur only the output image, not the frames used for inference
2. **Post-processing**: Apply after gaze detection, eye tracking, and all analytics
3. **Real-time capable**: Minimal latency impact on frame output
4. **Configurable**: Allow users to enable/disable and adjust intensity
5. **Effective anonymization**: Faces must not be recoverable from blurred output

## Techniques Evaluated

### 1. Pixelation (Mosaic Effect)

**Method**: Downsample ROI to small size, then upsample with nearest-neighbor interpolation.

```cpp
cv::Mat roi = frame(bbox);
cv::Mat small;
cv::resize(roi, small, cv::Size(block_size, block_size), 0, 0, cv::INTER_LINEAR);
cv::resize(small, roi, roi.size(), 0, 0, cv::INTER_NEAREST);
```

| Aspect | Rating |
|--------|--------|
| Speed | Excellent - O(1) relative to blur radius |
| Privacy | Excellent - complete anonymization |
| Appearance | Good - clearly intentional, recognizable style |
| Complexity | Very Low - ~5 lines of code |

**Pros**:
- Fastest option (two resize operations)
- Unambiguously intentional - viewers understand it's deliberate
- Block size easily configurable (8x8, 12x12, 16x16)
- No kernel size tuning needed

**Cons**:
- Artificial/digital appearance
- May not suit all aesthetic preferences

### 2. Gaussian Blur (OpenCV)

**Method**: Apply Gaussian convolution kernel to ROI.

```cpp
cv::Mat roi = frame(bbox);
int ksize = 99; // Must be odd
cv::GaussianBlur(roi, roi, cv::Size(ksize, ksize), 0);
```

| Aspect | Rating |
|--------|--------|
| Speed | Good - O(n) with separable filter optimization |
| Privacy | Good - requires large kernel (71+) for effectiveness |
| Appearance | Excellent - natural, smooth blur |
| Complexity | Low - single function call |

**Pros**:
- Natural, aesthetically pleasing result
- Well-understood, widely used
- OpenCV highly optimized implementation

**Cons**:
- Requires large kernel (71x71 to 99x99) for true anonymization
- Small kernels can potentially be reversed
- Slightly slower than pixelation for large kernels

### 3. Box Blur (Average Blur)

**Method**: Simple averaging filter.

```cpp
cv::Mat roi = frame(bbox);
cv::blur(roi, roi, cv::Size(ksize, ksize));
```

| Aspect | Rating |
|--------|--------|
| Speed | Excellent - fastest convolution-based blur |
| Privacy | Good - similar to Gaussian with large kernel |
| Appearance | Fair - less smooth than Gaussian |
| Complexity | Very Low |

**Pros**:
- Fastest convolution-based method
- Simple implementation

**Cons**:
- Visible box artifacts at edges
- Less visually appealing than Gaussian

### 4. Median Blur

**Method**: Replace each pixel with median of neighborhood.

```cpp
cv::Mat roi = frame(bbox);
cv::medianBlur(roi, roi, ksize); // ksize must be odd
```

| Aspect | Rating |
|--------|--------|
| Speed | Poor - O(n log n) per pixel |
| Privacy | Good |
| Appearance | Good - preserves some edges |
| Complexity | Low |

**Pros**:
- Good edge preservation
- Removes salt-and-pepper noise

**Cons**:
- Significantly slower than other methods
- Not ideal for anonymization use case

### 5. RGA Hardware Acceleration (Rockchip)

**Method**: Use Rockchip's 2D graphics accelerator.

| Aspect | Rating |
|--------|--------|
| Speed | Potentially excellent (hardware offload) |
| Privacy | Depends on supported operations |
| Appearance | Depends on supported operations |
| Complexity | High - RGA API integration |

**Cons**:
- RGA does not natively support blur operations
- Would require multiple blit operations to simulate
- Added complexity not justified for this use case
- Not portable to non-Rockchip platforms

**Verdict**: Not recommended for this application.

## Recommendation: Pixelation (Primary) with Gaussian Option

### Primary Method: Pixelation

**Rationale**:
1. **Performance**: Fastest method with negligible CPU impact
2. **Effectiveness**: Complete anonymization regardless of block size
3. **Clarity**: Obviously intentional - important for legal/compliance contexts
4. **Simplicity**: Minimal code, easy to maintain
5. **Configurability**: Single parameter (block size) controls intensity

### Secondary Method: Gaussian Blur

Offer as configurable alternative for users preferring smoother appearance.

## Proposed Implementation

### Configuration (config.json)

All blur settings are optional. If not specified, the following defaults apply:

```json
{
  "blur_faces": false,
  "blur_method": "pixelate",
  "blur_intensity": 12
}
```

To enable face blurring, simply add `"blur_faces": true` to your config. The method and intensity will use sensible defaults.

| Parameter | Values | Default | Description |
|-----------|--------|---------|-------------|
| `blur_faces` | `true`/`false` | `false` | Enable/disable face blurring |
| `blur_method` | `"pixelate"`, `"gaussian"` | `"pixelate"` | Blur algorithm |
| `blur_intensity` | 4-32 (pixelate), 31-99 (gaussian) | `12` | Blur strength |

### Code Location

**File**: `src/output/annotator.cpp` or new `src/output/face_blur.cpp`

**Integration Point**: After annotations are drawn, before frame is written.

```
Pipeline Flow:
  Capture → Preprocess → Inference → Postprocess → Analytics
                                                      ↓
                                              Annotate frame
                                                      ↓
                                              Blur faces ← NEW
                                                      ↓
                                              Write to /tmp
```

### Implementation Sketch

```cpp
// src/output/face_blur.hpp
#pragma once
#include <opencv2/opencv.hpp>
#include <vector>

enum class BlurMethod {
    PIXELATE,
    GAUSSIAN
};

struct BlurConfig {
    bool enabled = false;
    BlurMethod method = BlurMethod::PIXELATE;
    int intensity = 12;  // block_size for pixelate, kernel_size for gaussian
};

void blur_faces(cv::Mat& frame,
                const std::vector<cv::Rect>& face_bboxes,
                const BlurConfig& config);
```

```cpp
// src/output/face_blur.cpp
#include "face_blur.hpp"

void blur_faces(cv::Mat& frame,
                const std::vector<cv::Rect>& face_bboxes,
                const BlurConfig& config) {
    if (!config.enabled || face_bboxes.empty()) return;

    for (const auto& bbox : face_bboxes) {
        // Clamp bounding box to frame dimensions
        cv::Rect safe_bbox = bbox & cv::Rect(0, 0, frame.cols, frame.rows);
        if (safe_bbox.area() == 0) continue;

        cv::Mat roi = frame(safe_bbox);

        switch (config.method) {
            case BlurMethod::PIXELATE: {
                int block_size = std::clamp(config.intensity, 4, 32);
                cv::Mat small;
                cv::resize(roi, small, cv::Size(block_size, block_size),
                          0, 0, cv::INTER_LINEAR);
                cv::resize(small, roi, roi.size(),
                          0, 0, cv::INTER_NEAREST);
                break;
            }
            case BlurMethod::GAUSSIAN: {
                int ksize = config.intensity | 1;  // Ensure odd
                ksize = std::clamp(ksize, 31, 99);
                cv::GaussianBlur(roi, roi, cv::Size(ksize, ksize), 0);
                break;
            }
        }
    }
}
```

### Call Site (in frame_writer or annotator)

```cpp
// After drawing annotations, before imwrite:
if (blur_config.enabled) {
    // Extract bounding boxes from detection results
    std::vector<cv::Rect> face_bboxes;
    for (const auto& face : results.faces) {
        face_bboxes.push_back(face.bbox);
    }
    blur_faces(output_frame, face_bboxes, blur_config);
}

cv::imwrite(output_path, output_frame);
```

## Performance Estimate

For a 640x480 frame with 3 detected faces (average bbox 100x120):

| Method | Estimated Time | Impact |
|--------|----------------|--------|
| Pixelation (12x12) | ~0.1ms | Negligible |
| Gaussian (71x71) | ~0.8ms | Minimal |
| Gaussian (99x99) | ~1.5ms | Low |

Both methods are suitable for real-time operation at 30fps.

## Privacy Considerations

- **Pixelation**: Irrecoverable at any block size - information is destroyed
- **Gaussian blur**: Potentially recoverable with small kernels via deconvolution; use kernel size >= 71 for security-sensitive applications
- **Recommendation**: For compliance/legal requirements, prefer pixelation or large Gaussian kernels

## Future Enhancements

1. **Elliptical mask**: Blur only face oval, not rectangular bbox
2. **Feathered edges**: Smooth transition at blur boundary
3. **Body blur**: Extend to full-body anonymization
4. **Selective blur**: Blur only non-attending faces (privacy for bystanders)

## Conclusion

**Recommended approach**: Implement pixelation as the default blur method with Gaussian as a configurable alternative. The implementation is straightforward, performant, and provides effective privacy protection without impacting the detection pipeline.
