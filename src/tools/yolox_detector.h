// SPDX-License-Identifier: MIT
// YoloxDetector: the D4 automatic indexer's OWN YOLOX-S RKNN context (separate from the running Argus detector, which is
// untouched). Reuses Argus' yolox.cc / postprocess.cc so the model sees exactly the pre/post-processing the pipeline uses.
// Compiled only for the player build (ARGUS_OBJECT_DETECTOR). Single-thread use (the indexer worker).
#ifndef ARGUS_TOOLS_YOLOX_DETECTOR_H
#define ARGUS_TOOLS_YOLOX_DETECTOR_H

#include "visualmemory/object_detector.h"

#include <memory>
#include <string>

namespace argus_objects {

class YoloxDetector : public visualmemory::ObjectDetector {
public:
    struct Options {
        std::string model_path = "model/yolox_s.rknn";
        int         npu_core = 1;               // 0/1/2 single core, 3 = all, -1 = runtime default
    };
    explicit YoloxDetector(Options o);
    ~YoloxDetector() override;

    bool init();                                // loads the model once; false (last_error set) on failure
    bool ready() const override;
    std::string last_error() const override;
    std::string name() const override { return "yolox_s"; }
    std::string version() const override;      // model fingerprint (file size + FNV-1a of the bytes), set by init()
    // The post-processing keeps a 0.10 internal score floor (shared with Argus); `min_confidence` is applied on top, here.
    bool detect(const visualmemory::ImageView& rgb, float min_confidence, std::vector<visualmemory::DetectedObject>& out) override;
    double last_latency_ms() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace argus_objects

#endif
