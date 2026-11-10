// SPDX-License-Identifier: MIT
// Dev/test ONLY object detector: finds the saturated colour blob of a synthetic scene and names it by colour
// (red -> bottle, green -> cup, yellow -> laptop, blue -> person). Lets the automatic-indexing path, the API and the
// dashboard be exercised on a host with no NPU. Never used on a player unless started with --detector mock.
#ifndef ARGUS_TOOLS_MOCK_COLOUR_DETECTOR_H
#define ARGUS_TOOLS_MOCK_COLOUR_DETECTOR_H

#include "visualmemory/object_detector.h"

#include <algorithm>
#include <memory>

namespace argus_objects {

inline std::shared_ptr<visualmemory::ObjectDetector> make_colour_blob_detector() {
    using namespace visualmemory;
    return std::make_shared<MockObjectDetector>([](const ImageView& v) {
        std::vector<DetectedObject> out;
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, r = 0, g = 0;
        for (int y = 0; y < v.height; ++y) for (int x = 0; x < v.width; ++x) {
            const uint8_t* p = v.data + static_cast<size_t>(y) * (v.stride ? v.stride : v.width * 3) + static_cast<size_t>(x) * 3;
            const int mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
            if (mx - mn > 100) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); r = p[0]; g = p[1]; }
        }
        if (x1 < 0) return out;
        DetectedObject d; d.x = x0; d.y = y0; d.w = x1 - x0 + 1; d.h = y1 - y0 + 1;
        if (r > 150 && g > 150) { d.class_id = 63; d.confidence = 0.55f; }
        else if (r > 150 && g < 90) { d.class_id = 39; d.confidence = 0.90f; }
        else if (g > 150) { d.class_id = 41; d.confidence = 0.60f; }
        else { d.class_id = 0; d.confidence = 0.80f; }
        d.class_name = coco80_name(d.class_id);
        out.push_back(d);
        return out;
    });
}

} // namespace argus_objects

#endif
