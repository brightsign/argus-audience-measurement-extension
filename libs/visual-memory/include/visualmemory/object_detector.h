// SPDX-License-Identifier: MIT
// ObjectDetector: the narrow interface the D4 automatic object indexer needs. The concrete YOLOX/RKNN detector
// lives in the Argus tree (src/tools/yolox_detector.cpp) so libvisualmemory stays free of Argus and of RKNN; tests
// use MockObjectDetector. A detector is used by ONE thread (the indexer worker): implementations need not be
// thread safe, but must not block for longer than one inference.
#ifndef VISUALMEMORY_OBJECT_DETECTOR_H
#define VISUALMEMORY_OBJECT_DETECTOR_H

#include "visualmemory/types.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace visualmemory {

struct DetectedObject {
    int         class_id = -1;          // COCO-80 index
    std::string class_name;
    float       confidence = 0.0f;      // detector score, NOT an identification probability
    int         x = 0, y = 0, w = 0, h = 0;   // pixels of the image given to detect(), origin top-left
};

class ObjectDetector {
public:
    virtual ~ObjectDetector() = default;
    virtual bool ready() const = 0;
    virtual std::string last_error() const = 0;
    // Identifies the model for provenance, e.g. "yolox_s" / "<sha or file tag>".
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    // Detects objects in a packed RGB24 image. `min_confidence` is applied by the detector (it may keep a lower
    // internal floor). Output is sorted by descending confidence. Returns false on an inference error.
    virtual bool detect(const ImageView& rgb, float min_confidence, std::vector<DetectedObject>& out) = 0;
    // Optional diagnostics: time of the last detect() call in ms (0 if unknown).
    virtual double last_latency_ms() const { return 0.0; }
};

// COCO-80 class names in the model's index order (identical to the packaged coco_80_labels_list.txt; checked by a
// test against that file so a label-list change cannot silently shift class ids).
const std::vector<std::string>& coco80_names();
// "bottle" -> 39; returns -1 for an unknown name. Accepts the underscore spelling used by the packaged file.
int coco80_id(const std::string& name);
const char* coco80_name(int id);   // "" when out of range

// Test double: returns a fixed list for any image (or whatever `fn` produces).
class MockObjectDetector : public ObjectDetector {
public:
    using Fn = std::function<std::vector<DetectedObject>(const ImageView&)>;
    explicit MockObjectDetector(Fn fn = {}) : fn_(std::move(fn)) {}
    void set_ready(bool r) { ready_ = r; }
    void set_fail(bool f) { fail_ = f; }
    bool ready() const override { return ready_; }
    std::string last_error() const override { return fail_ ? "mock detector failure" : std::string{}; }
    std::string name() const override { return "mock_detector"; }
    std::string version() const override { return "1"; }
    bool detect(const ImageView& img, float min_confidence, std::vector<DetectedObject>& out) override {
        out.clear();
        ++calls;
        if (fail_ || !ready_) return false;
        if (fn_) for (const auto& d : fn_(img)) if (d.confidence >= min_confidence) out.push_back(d);
        return true;
    }
    int calls = 0;
private:
    Fn fn_; bool ready_ = true, fail_ = false;
};

} // namespace visualmemory

#endif // VISUALMEMORY_OBJECT_DETECTOR_H
