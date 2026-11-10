// SPDX-License-Identifier: MIT
#include "yolox_detector.h"

#include "yolox.h"                              // rknn_app_context_t, inference_yolox_model, init/release (Argus)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace argus_objects {

struct YoloxDetector::Impl {
    Options opt;
    rknn_app_context_t ctx{};
    bool loaded = false;
    std::string err, ver;
    double last_ms = 0.0;
    std::vector<unsigned char> repack;          // only used when the caller's stride is not tight
};

namespace {
// 64-bit FNV-1a over the model file: a stable identity for provenance (which model produced a detection).
std::string fingerprint(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return "unknown";
    uint64_t h = 1469598103934665603ULL, n = 0;
    unsigned char buf[65536];
    size_t r;
    while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) { for (size_t i = 0; i < r; ++i) { h ^= buf[i]; h *= 1099511628211ULL; } n += r; }
    std::fclose(f);
    char out[48]; std::snprintf(out, sizeof out, "%llu-%016llx", static_cast<unsigned long long>(n), static_cast<unsigned long long>(h));
    return out;
}
} // namespace

YoloxDetector::YoloxDetector(Options o) : impl_(std::make_unique<Impl>()) { impl_->opt = std::move(o); }

YoloxDetector::~YoloxDetector() {
    if (impl_->loaded) release_yolox_model(&impl_->ctx);
}

bool YoloxDetector::init() {
    Impl& d = *impl_;
    if (d.loaded) return true;
    std::memset(&d.ctx, 0, sizeof d.ctx);
    if (init_yolox_model(d.opt.model_path.c_str(), &d.ctx, d.opt.npu_core) != 0) {
        d.err = "init_yolox_model failed for " + d.opt.model_path;
        release_yolox_model(&d.ctx);
        return false;
    }
    d.ver = fingerprint(d.opt.model_path);
    d.loaded = true; d.err.clear();
    return true;
}

bool YoloxDetector::ready() const { return impl_->loaded; }
std::string YoloxDetector::last_error() const { return impl_->err; }
std::string YoloxDetector::version() const { return impl_->ver; }
double YoloxDetector::last_latency_ms() const { return impl_->last_ms; }

bool YoloxDetector::detect(const visualmemory::ImageView& v, float min_confidence, std::vector<visualmemory::DetectedObject>& out) {
    Impl& d = *impl_;
    out.clear();
    if (!d.loaded) { d.err = "detector not loaded"; return false; }
    if (!v.data || v.width < 16 || v.height < 16 || v.format != visualmemory::PixelFormat::RGB24) { d.err = "unsupported image (RGB24 required)"; return false; }
    const int tight = v.width * 3;
    const unsigned char* px = v.data;
    if (v.stride != 0 && v.stride != tight) {
        d.repack.resize(static_cast<size_t>(tight) * v.height);
        for (int y = 0; y < v.height; ++y) std::memcpy(d.repack.data() + static_cast<size_t>(y) * tight, v.data + static_cast<size_t>(y) * v.stride, static_cast<size_t>(tight));
        px = d.repack.data();
    }
    image_buffer_t img;
    std::memset(&img, 0, sizeof img);
    img.width = v.width; img.height = v.height; img.width_stride = v.width; img.height_stride = v.height;
    img.format = IMAGE_FORMAT_RGB888; img.virt_addr = const_cast<unsigned char*>(px); img.size = tight * v.height; img.fd = -1;

    object_detect_result_list res;
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = inference_yolox_model(&d.ctx, &img, &res, min_confidence);
    d.last_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rc != 0) { d.err = "inference_yolox_model failed (" + std::to_string(rc) + ")"; return false; }
    d.err.clear();
    for (int i = 0; i < res.count; ++i) {
        const auto& r = res.results[i];
        if (r.prop < min_confidence) continue;
        visualmemory::DetectedObject o;
        o.class_id = r.cls_id; o.class_name = visualmemory::coco80_name(r.cls_id); o.confidence = r.prop;
        o.x = r.box.left; o.y = r.box.top; o.w = r.box.right - r.box.left; o.h = r.box.bottom - r.box.top;
        if (o.w > 0 && o.h > 0) out.push_back(o);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.confidence > b.confidence; });
    return true;
}

} // namespace argus_objects
