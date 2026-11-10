// SPDX-License-Identifier: MIT
// vm_object_probe: controlled, READ-ONLY feasibility probe for D4 automatic object indexing.
//
// For a list of stored frames it
//   * reads their JPEGs through the read-only VisualMemoryReader (visual_memory.db and objects.db are never written),
//   * runs the detector (YOLOX-S RKNN on the player) at several worker-side thresholds, repeated N times for latency,
//   * crops the allowlisted detections (crop JPEGs go to --crop-dir only: they may show people, keep them off Git),
//   * embeds the crops with the existing MobileNetV2 provider and prints the cosine-similarity matrix,
//   * reports detector / embedding latency, process CPU time, RSS and the wall-clock of the whole run.
// Output is one JSON document on stdout (or --out). It does not touch Argus, the running presentation or the
// configuration. Run it from a shell on the player (see docs/VISUAL_MEMORY_OBJECT_AUTO_INDEXING.md).
#include <3rdparty/nlohmann/json.hpp>

#include "visualmemory/mock_embedding_provider.h"
#include "visualmemory/object_detector.h"
#include "visualmemory/reader.h"
#include "embedding_math.h"
#include "jpeg_decoder.h"
#include "mock_colour_detector.h"
#ifdef ARGUS_OBJECT_DETECTOR
#include "yolox_detector.h"
#endif
#ifdef ARGUS_VISUAL_EMBEDDINGS
#include "rknn_embedding_provider.h"
#endif

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using json = nlohmann::json;
using namespace visualmemory;

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

json stats(std::vector<double> v) {
    if (v.empty()) return json::object();
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))]; };
    double sum = 0; for (double x : v) sum += x;
    return {{"n", v.size()}, {"min", v.front()}, {"p50", pct(0.5)}, {"p95", pct(0.95)}, {"max", v.back()}, {"mean", sum / v.size()}};
}
long rss_kb(const char* key) {
    std::ifstream f("/proc/self/status"); std::string line;
    while (std::getline(f, line)) if (line.rfind(key, 0) == 0) return std::atol(line.c_str() + std::strlen(key) + 1);
    return -1;
}
double cpu_seconds() { rusage u{}; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6; }
std::vector<std::string> split(const std::string& s, char c) { std::vector<std::string> o; std::stringstream ss(s); std::string t; while (std::getline(ss, t, c)) if (!t.empty()) o.push_back(t); return o; }

struct Crop { int64_t frame; int idx; std::string cls; float conf; int x, y, w, h; std::vector<float> emb; std::string file; };
} // namespace

int main(int argc, char** argv) {
    std::string root = "/storage/sd/visual-memory", base_dir, frames_arg, thr_arg = "0.5,0.3,0.25", classes_arg = "bottle,cup,wine_glass,laptop,cell_phone,book";
    std::string jpeg_arg;                                    // --jpeg a.jpg,b.jpg : probe JPEG files directly (e.g. display frames fetched from the dashboard API)
    std::string crop_dir, out_path, detector_kind = "auto", provider_kind = "auto", embed_model = "model/visual_memory/mobilenet_embedder.rknn", det_model = "model/yolox_s.rknn";
    int core = 1, embed_core = 1, repeat = 10, crop_pad = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--root") root = next(); else if (a == "--base-dir") base_dir = next(); else if (a == "--frames") frames_arg = next();
        else if (a == "--jpeg") jpeg_arg = next(); else if (a == "--thresholds") thr_arg = next(); else if (a == "--classes") classes_arg = next(); else if (a == "--crop-dir") crop_dir = next();
        else if (a == "--out") out_path = next(); else if (a == "--detector") detector_kind = next(); else if (a == "--provider") provider_kind = next();
        else if (a == "--core") core = std::atoi(next().c_str()); else if (a == "--embed-core") embed_core = std::atoi(next().c_str());
        else if (a == "--repeat") repeat = std::max(1, std::min(200, std::atoi(next().c_str())));
        else if (a == "--det-model") det_model = next(); else if (a == "--embed-model") embed_model = next(); else if (a == "--crop-pad") crop_pad = std::max(0, std::atoi(next().c_str()));
        else { std::fprintf(stderr, "usage: vm_object_probe --frames 84748,84749 [--root DIR] [--base-dir EXTDIR] [--thresholds 0.5,0.3,0.25] [--classes bottle,cup,...]\n"
                                    "       [--core N] [--embed-core N] [--repeat N] [--crop-dir DIR] [--out FILE] [--detector auto|mock] [--provider auto|mock]\n"); return 2; }
    }
    std::vector<int64_t> frames; for (const auto& s : split(frames_arg, ',')) frames.push_back(std::atoll(s.c_str()));
    std::vector<float> thresholds; for (const auto& s : split(thr_arg, ',')) thresholds.push_back(static_cast<float>(std::atof(s.c_str())));
    std::vector<int> want; for (const auto& s : split(classes_arg, ',')) { const int id = coco80_id(s); if (id >= 0) want.push_back(id); }
    const std::vector<std::string> jpeg_files = split(jpeg_arg, ',');
    if ((frames.empty() && jpeg_files.empty()) || thresholds.empty()) { std::fprintf(stderr, "vm_object_probe: --frames (stored camera frames) and/or --jpeg (files), and --thresholds are required\n"); return 2; }
    const float low = *std::min_element(thresholds.begin(), thresholds.end());
    auto resolve = [&](const std::string& p) { return p.rfind('/', 0) == 0 || base_dir.empty() ? p : base_dir + "/" + p; };

    json out = {{"tool", "vm_object_probe"}, {"read_only", true}, {"frames_requested", frames}, {"thresholds", thresholds}, {"classes", classes_arg}};
    const auto t_run = Clock::now(); const double cpu0 = cpu_seconds(); const long rss0 = rss_kb("VmRSS:");

    VisualMemoryReader reader(ReaderConfig{root});
    std::string err;
    if (!frames.empty() && !reader.open(err)) { out["error"] = "frame store: " + err; std::puts(out.dump(1).c_str()); return 1; }

    // ---- models
    std::shared_ptr<ObjectDetector> det;
    if (detector_kind == "mock") det = argus_objects::make_colour_blob_detector();
#ifdef ARGUS_OBJECT_DETECTOR
    else {
        argus_objects::YoloxDetector::Options o; o.model_path = resolve(det_model); o.npu_core = core;
        auto d = std::make_shared<argus_objects::YoloxDetector>(o);
        const auto t = Clock::now();
        if (!d->init()) { out["error"] = "detector: " + d->last_error(); std::puts(out.dump(1).c_str()); return 1; }
        out["detector_load_ms"] = ms_since(t); det = d;
    }
#endif
    if (!det) { out["error"] = "no detector available in this build (use --detector mock on a host)"; std::puts(out.dump(1).c_str()); return 1; }
    out["detector"] = {{"name", det->name()}, {"version", det->version()}, {"kind", detector_kind}, {"npu_core", core},
                       {"note", "post-processing keeps Argus' 0.10 internal score floor; thresholds here are applied on top"}};

    std::shared_ptr<EmbeddingProvider> prov;
    if (provider_kind == "mock") prov = std::make_shared<MockEmbeddingProvider>(64);
#ifdef ARGUS_VISUAL_EMBEDDINGS
    else {
        RknnEmbeddingProvider::Options ro; ro.model_path = resolve(embed_model); ro.npu_core = embed_core;
        auto p = std::make_shared<RknnEmbeddingProvider>(ro);
        const auto t = Clock::now();
        if (!p->init()) { out["error"] = "embedding provider: " + p->last_error(); std::puts(out.dump(1).c_str()); return 1; }
        out["embedder_load_ms"] = ms_since(t); prov = p;
    }
#endif
    if (!prov) { out["error"] = "no embedding provider in this build (use --provider mock on a host)"; std::puts(out.dump(1).c_str()); return 1; }
    out["embedder"] = {{"model", prov->spec().model_name}, {"version", prov->spec().model_version}, {"dimension", prov->spec().dimension}, {"npu_core", embed_core}};
    if (!crop_dir.empty()) std::filesystem::create_directories(crop_dir);

    // ---- per frame
    json jf = json::array(); std::vector<Crop> crops; std::vector<double> all_detect_ms, all_embed_ms;
    struct Item { int64_t id; std::string file; };
    std::vector<Item> items; for (int64_t id : frames) items.push_back({id, ""}); for (const auto& f : jpeg_files) items.push_back({0, f});
    for (const auto& item : items) {
        const int64_t id = item.id;
        json row = {{"frame_id", id}};
        std::vector<uint8_t> jpeg;
        if (!item.file.empty()) {                                  // a JPEG file: nothing but the picture itself is read
            row["file"] = item.file;
            std::ifstream f(item.file, std::ios::binary); jpeg.assign(std::istreambuf_iterator<char>(f), {});
            if (jpeg.empty()) { row["error"] = "file unreadable"; jf.push_back(row); continue; }
        } else {
            FrameDetail fd;
            if (!reader.get_frame(id, fd)) { row["error"] = "unknown frame"; jf.push_back(row); continue; }
            row["capture_utc_ms"] = fd.record.frame.metadata.capture_utc_ms; row["stored_width"] = fd.record.frame.width; row["stored_height"] = fd.record.frame.height;
            row["jpeg_retained"] = fd.record.frame.image_available && fd.record.image_exists;
            const int rc = reader.read_jpeg(id, jpeg, err);
            if (rc != 0) { row["error"] = rc == 2 ? "frame JPEG has expired (nothing to detect in)" : "frame JPEG unreadable"; jf.push_back(row); continue; }
        }
        detail::DecodedImage img;
        if (!detail::decode_jpeg_rgb(jpeg.data(), jpeg.size(), img, err)) { row["error"] = "decode: " + err; jf.push_back(row); continue; }
        ImageView v; v.data = img.rgb.data(); v.width = img.width; v.height = img.height; v.stride = img.width * 3; v.format = PixelFormat::RGB24;
        json byt = json::object();
        std::vector<DetectedObject> low_dets;
        for (float t : thresholds) {
            std::vector<double> lat; std::vector<DetectedObject> d;
            bool ok = true;
            for (int k = 0; k < repeat + 1; ++k) {                       // the first call (warm-up) is reported separately
                if (!det->detect(v, t, d)) { ok = false; break; }
                lat.push_back(det->last_latency_ms() > 0 ? det->last_latency_ms() : 0.0);
            }
            if (!ok) { byt[std::to_string(t)] = {{"error", det->last_error()}}; continue; }
            const double warm = lat.empty() ? 0 : lat.front(); if (!lat.empty()) lat.erase(lat.begin());
            all_detect_ms.insert(all_detect_ms.end(), lat.begin(), lat.end());
            json dj = json::array(); json others = json::array();
            for (const auto& o : d) {
                json e = {{"class_id", o.class_id}, {"class", o.class_name}, {"confidence", o.confidence}, {"bbox", {{"x", o.x}, {"y", o.y}, {"w", o.w}, {"h", o.h}}}};
                if (std::find(want.begin(), want.end(), o.class_id) != want.end()) dj.push_back(e); else if (others.size() < 6) others.push_back(e);
            }
            byt[std::to_string(t)] = {{"allowlisted_detections", dj}, {"other_classes_top", others}, {"detect_ms", stats(lat)}, {"first_call_ms", warm}};
            if (t == low) low_dets = d;
        }
        row["by_threshold"] = byt;
        // crops of the allowlisted detections at the LOWEST threshold; embeddings for the similarity matrix
        int k = 0;
        for (const auto& o : low_dets) {
            if (std::find(want.begin(), want.end(), o.class_id) == want.end()) continue;
            Crop c{item.file.empty() ? id : -1 - static_cast<int64_t>(&item - &items[0]), k++, o.class_name, o.confidence, std::max(0, o.x - crop_pad), std::max(0, o.y - crop_pad), 0, 0, {}, {}};
            c.w = std::min(img.width - c.x, o.w + 2 * crop_pad); c.h = std::min(img.height - c.y, o.h + 2 * crop_pad);
            if (c.w < 8 || c.h < 8) continue;
            std::vector<uint8_t> px(static_cast<size_t>(c.w) * c.h * 3);
            for (int y = 0; y < c.h; ++y) std::copy_n(img.rgb.data() + (static_cast<size_t>(c.y + y) * img.width + c.x) * 3, static_cast<size_t>(c.w) * 3, px.data() + static_cast<size_t>(y) * c.w * 3);
            ImageView cv; cv.data = px.data(); cv.width = c.w; cv.height = c.h; cv.stride = c.w * 3; cv.format = PixelFormat::RGB24;
            std::vector<double> el;
            bool ok = false;
            for (int r = 0; r < std::min(repeat, 5) + 1; ++r) { const auto t = Clock::now(); ok = prov->embed(cv, c.emb); el.push_back(ms_since(t)); if (!ok) break; }
            if (!ok || detail::l2_normalize(c.emb, prov->spec().dimension) != detail::VecCheck::Ok) continue;
            if (!el.empty()) el.erase(el.begin());
            all_embed_ms.insert(all_embed_ms.end(), el.begin(), el.end());
            if (!crop_dir.empty()) {
                std::vector<uint8_t> cj; std::string ce;
                if (detail::encode_rgb_jpeg(px.data(), c.w, c.h, 90, cj, ce)) {
                    c.file = crop_dir + "/frame" + (item.file.empty() ? std::to_string(id) : "file" + std::to_string(&item - &items[0])) + "_" + std::to_string(c.idx) + "_" + c.cls + ".jpg";
                    std::ofstream f(c.file, std::ios::binary); f.write(reinterpret_cast<const char*>(cj.data()), static_cast<std::streamsize>(cj.size()));
                }
            }
            crops.push_back(std::move(c));
        }
        jf.push_back(row);
    }
    out["frames"] = jf;

    // ---- similarity between the crops (cosine of L2-normalised vectors; NOT identification probabilities)
    json cj = json::array(); for (size_t i = 0; i < crops.size(); ++i) { const auto& c = crops[i]; cj.push_back({{"n", i}, {"frame_id", c.frame}, {"class", c.cls}, {"confidence", c.conf}, {"bbox", {{"x", c.x}, {"y", c.y}, {"w", c.w}, {"h", c.h}}}, {"crop_file", c.file}}); }
    json mx = json::array();
    for (size_t i = 0; i < crops.size(); ++i) {
        json r = json::array();
        for (size_t j = 0; j < crops.size(); ++j) { double d = 0; for (size_t k = 0; k < crops[i].emb.size(); ++k) d += static_cast<double>(crops[i].emb[k]) * crops[j].emb[k]; r.push_back(std::round(d * 10000) / 10000); }
        mx.push_back(r);
    }
    out["crops"] = cj; out["similarity_matrix"] = mx;
    out["embed_ms"] = stats(all_embed_ms); out["detect_ms_all"] = stats(all_detect_ms);
    out["resources"] = {{"wall_s", ms_since(t_run) / 1000.0}, {"cpu_s", cpu_seconds() - cpu0}, {"rss_kb_start", rss0}, {"rss_kb_end", rss_kb("VmRSS:")}, {"rss_kb_peak", rss_kb("VmHWM:")}};
    const std::string text = out.dump(1);
    if (!out_path.empty()) { std::ofstream f(out_path); f << text << "\n"; } else std::puts(text.c_str());
    return 0;
}
