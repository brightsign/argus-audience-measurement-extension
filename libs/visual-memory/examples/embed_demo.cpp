// SPDX-License-Identifier: MIT
//
// vm_embed_demo — Stage 2 command-line demo and utility (runs on x86 hosts with the mock
// provider and on the XT5 with the RKNN provider; no Python).
//
//   embed    --image a.jpg [--provider rknn --model m.rknn] [--runs N] [--out vec.f32]
//   embed    --dir D --out-dir O [--provider rknn --model m.rknn] [--runs N]   (one raw .f32 per image + latency)
//   ingest   --root R --dir D [--manifest manifest.json] [--provider ...] [--interval-ms 5000] [--no-embed]
//   search   --root R --query q.jpg [--top 5] [--source S] [--start-ms A --end-ms B] [--min-sim x]
//   backfill --root R [--max N] [--provider ...]
//   stats    --root R
//   bench-db --root R [--sizes 100,1000,10000] [--dim 1280]       (synthetic vectors, search latency)
//
// Common: --provider mock|rknn (default mock), --model <path.rknn>, --npu-core N, --no-vec (BLOB fallback)
#include "visualmemory/mock_embedding_provider.h"
#include "visualmemory/preprocess.h"
#include "visualmemory/visual_memory.h"
#ifdef VM_WITH_RKNN
#include "rknn_embedding_provider.h"
#endif

#include <turbojpeg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <thread>

using namespace visualmemory;
namespace fs = std::filesystem;

namespace {

struct Args {
    std::string cmd;
    std::map<std::string, std::string> kv;
    bool has(const std::string& k) const { return kv.count(k) > 0; }
    std::string get(const std::string& k, const std::string& d = {}) const {
        auto it = kv.find(k); return it == kv.end() ? d : it->second; }
    long long num(const std::string& k, long long d) const { return has(k) ? std::atoll(get(k).c_str()) : d; }
};

Args parse(int argc, char** argv) {
    Args a;
    if (argc > 1) a.cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0) continue;
        k = k.substr(2);
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) a.kv[k] = argv[++i];
        else a.kv[k] = "1";
    }
    return a;
}

bool read_file(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

bool decode_rgb(const std::vector<uint8_t>& jpeg, int& w, int& h, std::vector<uint8_t>& rgb) {
    tjhandle tj = tjInitDecompress();
    if (!tj) return false;
    int ss, cs;
    bool ok = tjDecompressHeader3(tj, jpeg.data(), jpeg.size(), &w, &h, &ss, &cs) == 0 && w > 0 && h > 0;
    if (ok) { rgb.resize(static_cast<size_t>(w) * h * 3);
              ok = tjDecompress2(tj, jpeg.data(), jpeg.size(), rgb.data(), w, 0, h, TJPF_RGB, 0) == 0; }
    tjDestroy(tj);
    return ok;
}

std::string iso(int64_t ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

std::shared_ptr<EmbeddingProvider> make_provider(const Args& a, std::string& note) {
    const std::string which = a.get("provider", "mock");
    if (which == "mock") { note = "mock provider (deterministic thumbnail projection; NOT a quality model)";
                           return std::make_shared<MockEmbeddingProvider>(64); }
#ifdef VM_WITH_RKNN
    if (which == "rknn") {
        RknnEmbeddingProvider::Options o;
        o.model_path = a.get("model");
        o.npu_core = static_cast<int>(a.num("npu-core", -1));
        auto p = std::make_shared<RknnEmbeddingProvider>(o);
        if (!p->init()) { std::fprintf(stderr, "rknn init failed: %s\n", p->last_error().c_str()); return nullptr; }
        note = "rknn: " + p->describe();
        return p;
    }
#endif
    std::fprintf(stderr, "unknown or unavailable provider '%s'\n", which.c_str());
    return nullptr;
}

Config make_cfg(const Args& a, bool embeddings) {
    Config c;
    c.root_directory = a.get("root");
    c.embedding.enabled = embeddings;
    c.embedding.prefer_sqlite_vec = !a.has("no-vec");
    c.embedding.max_pending = 64;
    c.embedding.backfill_pause_ms = 0;
    // Demo/utility defaults: NO expiry. Demo datasets carry synthetic (old) timestamps that the 48 h / 7 d
    // production retention would delete on start. Pass explicit values to exercise retention.
    c.retention_hours = static_cast<int>(a.num("retention-hours", 0));
    c.embedding.retention_days = static_cast<int>(a.num("retention-days", 0));
    c.max_storage_bytes = static_cast<uint64_t>(a.num("max-bytes", 0));
    return c;
}

// Minimal parser for scripts/make_demo_dataset.py manifest.json: filename -> synthetic_timestamp_ms.
std::map<std::string, int64_t> load_manifest(const std::string& path) {
    std::map<std::string, int64_t> m;
    std::ifstream f(path);
    std::string line, cur;
    while (std::getline(f, line)) {
        auto q1 = line.find('"');
        if (q1 == std::string::npos) continue;
        auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string key = line.substr(q1 + 1, q2 - q1 - 1);
        if (key.size() > 4 && key.substr(key.size() - 4) == ".jpg" && line.find('{') != std::string::npos) cur = key;
        else if (key == "synthetic_timestamp_ms" && !cur.empty()) {
            m[cur] = std::atoll(line.c_str() + line.find(':') + 1);
            cur.clear();
        }
    }
    return m;
}

void print_stats(VisualMemory& vm) {
    const Stats s = vm.stats();
    const EmbeddingStats e = vm.embedding_stats();
    std::printf("stage1: accepted=%llu encoded=%llu dropped_full=%llu write_fail=%llu db_fail=%llu bytes_on_disk=%llu\n",
                (unsigned long long)s.accepted, (unsigned long long)s.encoded, (unsigned long long)s.dropped_queue_full,
                (unsigned long long)s.jpeg_write_failures, (unsigned long long)s.database_failures,
                (unsigned long long)s.bytes_on_disk);
    std::printf("stage2: status=%s backend=%s dim=%d vectors=%llu vec_bytes=%llu embedded=%llu backfilled=%llu "
                "dropped_full=%llu inference_fail=%llu invalid=%llu decode_fail=%llu perm_fail=%llu db_fail=%llu q_max=%zu\n",
                e.status.c_str(), e.backend.c_str(), e.dimension, (unsigned long long)e.vectors_stored,
                (unsigned long long)e.vector_bytes, (unsigned long long)e.embedded, (unsigned long long)e.backfilled,
                (unsigned long long)e.dropped_queue_full, (unsigned long long)e.inference_failures,
                (unsigned long long)e.invalid_vectors, (unsigned long long)e.decode_failures,
                (unsigned long long)e.failed_permanent, (unsigned long long)e.db_failures, e.max_queue_depth);
    auto L = [](const char* n, const LatencyStats& l) {
        std::printf("  %-14s n=%llu p50=%.2f p95=%.2f p99=%.2f max=%.2f ms\n", n, (unsigned long long)l.count,
                    l.p50_ms, l.p95_ms, l.p99_ms, l.max_ms); };
    L("file_read", e.file_read); L("jpeg_decode", e.jpeg_decode); L("preprocess", e.preprocess);
    L("inference", e.inference); L("vector_insert", e.vector_insert); L("process_total", e.process_total);
    L("search", e.search);
}

int cmd_embed_dir(const Args& a, EmbeddingProvider& p) {
    fs::create_directories(a.get("out-dir"));
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(a.get("dir"))) {
        auto ext = e.path().extension().string();
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".JPG") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    const int runs = static_cast<int>(a.num("runs", 1));
    std::vector<double> ms;
    size_t n = 0;
    for (auto& f : files) {
        std::vector<uint8_t> jpeg; int w = 0, h = 0; std::vector<uint8_t> rgb;
        if (!read_file(f.string(), jpeg) || !decode_rgb(jpeg, w, h, rgb)) { std::fprintf(stderr, "skip %s\n", f.c_str()); continue; }
        ImageView v; v.data = rgb.data(); v.width = w; v.height = h; v.stride = w * 3; v.format = PixelFormat::RGB24;
        std::vector<float> vec;
        for (int i = 0; i < runs; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            if (!p.embed(v, vec)) { std::fprintf(stderr, "embed failed for %s: %s\n", f.c_str(), p.last_error().c_str()); return 3; }
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::ofstream o((fs::path(a.get("out-dir")) / (f.stem().string() + ".f32")).string(), std::ios::binary);
        o.write(reinterpret_cast<const char*>(vec.data()), vec.size() * sizeof(float));
        ++n;
    }
    std::sort(ms.begin(), ms.end());
    auto pct = [&](double q) { return ms[std::min(ms.size() - 1, static_cast<size_t>(std::ceil(q * ms.size())) - 1)]; };
    std::printf("embedded %zu image(s) x %d run(s) -> %s\n", n, runs, a.get("out-dir").c_str());
    std::printf("embed wall latency (n=%zu): p50=%.2f p95=%.2f p99=%.2f max=%.2f ms\n", ms.size(), pct(.5), pct(.95), pct(.99), ms.back());
    const auto ps = p.provider_stats();
    std::printf("provider: preprocess p50=%.2f p95=%.2f p99=%.2f | inference p50=%.2f p95=%.2f p99=%.2f | post p50=%.3f ms\n",
                ps.preprocess.p50_ms, ps.preprocess.p95_ms, ps.preprocess.p99_ms, ps.inference.p50_ms,
                ps.inference.p95_ms, ps.inference.p99_ms, ps.postprocess.p50_ms);
    return 0;
}

int cmd_embed(const Args& a) {
    std::string note;
    auto p = make_provider(a, note);
    if (!p) return 2;
    std::printf("%s\n", note.c_str());
    if (a.has("dir")) { int rc = cmd_embed_dir(a, *p); p->shutdown(); return rc; }
    std::vector<uint8_t> jpeg; int w = 0, h = 0; std::vector<uint8_t> rgb;
    if (!read_file(a.get("image"), jpeg) || !decode_rgb(jpeg, w, h, rgb)) { std::fprintf(stderr, "cannot read/decode --image\n"); return 2; }
    ImageView v; v.data = rgb.data(); v.width = w; v.height = h; v.stride = w * 3; v.format = PixelFormat::RGB24;
    std::vector<float> vec;
    const int runs = static_cast<int>(a.num("runs", 1));
    std::vector<double> ms;
    for (int i = 0; i < runs; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        if (!p->embed(v, vec)) { std::fprintf(stderr, "embed failed: %s\n", p->last_error().c_str()); return 3; }
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    double n = 0; for (float x : vec) n += double(x) * x;
    std::printf("dim=%zu raw_l2=%.6f first=[%.5f %.5f %.5f %.5f]\n", vec.size(), std::sqrt(n), vec[0], vec[1], vec[2], vec[3]);
    std::sort(ms.begin(), ms.end());
    auto pct = [&](double q) { return ms[std::min(ms.size() - 1, size_t(std::ceil(q * ms.size())) - (ms.size() ? 1 : 0))]; };
    std::printf("embed wall latency over %d run(s): p50=%.2f p95=%.2f p99=%.2f max=%.2f ms\n", runs, pct(.5), pct(.95), pct(.99), ms.back());
    const auto ps = p->provider_stats();
    std::printf("provider: preprocess p50=%.2f p95=%.2f | inference p50=%.2f p95=%.2f p99=%.2f | post p50=%.3f ms\n",
                ps.preprocess.p50_ms, ps.preprocess.p95_ms, ps.inference.p50_ms, ps.inference.p95_ms, ps.inference.p99_ms, ps.postprocess.p50_ms);
    if (a.has("out")) { std::ofstream o(a.get("out"), std::ios::binary); o.write(reinterpret_cast<const char*>(vec.data()), vec.size() * sizeof(float)); }
    if (a.has("dump-tensor")) {   // pp1 input tensor, for diffing against scripts/reference_embedder.py dump-tensor
        Pp1Preprocessor pp; std::vector<float> t(kPp1Floats); std::string err;
        if (!pp.run(v, t.data(), err)) { std::fprintf(stderr, "pp1: %s\n", err.c_str()); return 3; }
        std::ofstream o(a.get("dump-tensor"), std::ios::binary); o.write(reinterpret_cast<const char*>(t.data()), t.size() * sizeof(float));
    }
    p->shutdown();
    return 0;
}

int cmd_ingest(const Args& a) {
    std::string note;
    const bool emb = !a.has("no-embed");          // Stage-1-only ingest, e.g. to test backfill later
    std::shared_ptr<EmbeddingProvider> p;
    if (emb) { p = make_provider(a, note); if (!p) return 2; std::printf("%s\n", note.c_str()); }
    VisualMemory vm(make_cfg(a, emb));
    if (emb) vm.set_embedding_provider(p);
    if (!vm.start()) { std::fprintf(stderr, "start failed\n"); return 2; }
    std::printf("storage: %s | vector backend: %s\n", a.get("root").c_str(), vm.embedding_stats().backend.c_str());
    std::map<std::string, int64_t> man = a.has("manifest") ? load_manifest(a.get("manifest")) : std::map<std::string, int64_t>{};
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(a.get("dir"))) {
        auto ext = e.path().extension().string();
        if (ext != ".jpg" && ext != ".jpeg" && ext != ".JPG") continue;
        // With a manifest, ingest only the listed frames (reference.jpg etc. are queries, not history).
        if (!man.empty() && !man.count(e.path().filename().string())) continue;
        files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    const int64_t start = a.num("start-ms", 1767261600000LL), step = a.num("interval-ms", 5000);
    uint64_t seq = 0; size_t submitted = 0;
    for (auto& f : files) {
        std::vector<uint8_t> jpeg; int w = 0, h = 0; std::vector<uint8_t> rgb;
        if (!read_file(f.string(), jpeg) || !decode_rgb(jpeg, w, h, rgb)) { std::fprintf(stderr, "skip %s\n", f.c_str()); continue; }
        OwnedFrame fr;
        fr.width = w; fr.height = h; fr.pixel_format = PixelFormat::JPEG; fr.pixels = std::move(jpeg);   // stored verbatim
        fr.metadata.source_id = a.get("source", "demo-camera"); fr.metadata.source_type = "file";
        fr.metadata.device_id = "demo"; ++seq;
        auto it = man.find(f.filename().string());
        fr.metadata.capture_utc_ms = it != man.end() ? it->second : start + int64_t(seq - 1) * step;
        fr.metadata.source_sequence = seq;
        // Respect the bounded queue: retry briefly instead of dropping (demo tool, not the capture path).
        while (vm.stats().queue_depth >= 4) std::this_thread::sleep_for(std::chrono::milliseconds(2));   // Stage 1 queue depth is 4
        vm.try_submit(std::move(fr));
        ++submitted;
    }
    for (int i = 0; i < 6000; ++i) {   // wait for Stage 1 + Stage 2 queues to drain
        const Stats s = vm.stats(); const EmbeddingStats e = vm.embedding_stats();
        if (s.encoded >= submitted && (!emb || (e.embedded + e.failed_permanent >= submitted && e.queue_depth == 0))) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    print_stats(vm);
    vm.stop();
    return 0;
}

int cmd_search(const Args& a) {
    std::string note;
    auto p = make_provider(a, note);
    if (!p) return 2;
    VisualMemory vm(make_cfg(a, true));
    vm.set_embedding_provider(p);
    if (!vm.start()) { std::fprintf(stderr, "start failed\n"); return 2; }
    std::vector<uint8_t> jpeg;
    if (!read_file(a.get("query"), jpeg)) { std::fprintf(stderr, "cannot read --query\n"); return 2; }
    SearchOptions o;
    o.top_k = static_cast<int>(a.num("top", 5));
    o.start_utc_ms = a.num("start-ms", 0); o.end_utc_ms = a.num("end-ms", 0);
    o.source_id = a.get("source");
    if (a.has("min-sim")) { o.has_min_similarity = true; o.min_similarity = std::strtof(a.get("min-sim").c_str(), nullptr); }
    SearchStatus st;
    auto t0 = std::chrono::steady_clock::now();
    auto res = vm.search_by_jpeg(jpeg, o, &st);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!st.ok()) { std::fprintf(stderr, "search failed: %s\n", st.message.c_str()); return 3; }
    std::printf("Query: %s   (%s; scanned %llu vectors; end-to-end %.1f ms incl. query embedding)\n\n",
                fs::path(a.get("query")).filename().c_str(), vm.embedding_stats().backend.c_str(),
                (unsigned long long)st.candidates_scanned, ms);
    std::printf("%-5s %-12s %-10s %-20s %s\n", "Rank", "Frame", "Similarity", "Timestamp (UTC)", "Image");
    int rank = 0;
    for (const auto& r : res)
        std::printf("%-5d %-12lld %-10.4f %-20s %s%s\n", ++rank, (long long)r.frame_id, r.similarity,
                    iso(r.metadata.capture_utc_ms).c_str(), r.relative_jpeg_path.c_str(),
                    r.image_exists ? "" : "  [image unavailable]");
    vm.stop();
    return 0;
}

int cmd_backfill(const Args& a) {
    std::string note;
    auto p = make_provider(a, note);
    if (!p) return 2;
    std::printf("%s\n", note.c_str());
    VisualMemory vm(make_cfg(a, true));
    vm.set_embedding_provider(p);
    if (!vm.start()) { std::fprintf(stderr, "start failed\n"); return 2; }
    const size_t n = vm.backfill_embeddings(static_cast<size_t>(a.num("max", 1000)), a.num("deadline-ms", 0));
    std::printf("backfilled %zu frame(s)\n", n);
    print_stats(vm);
    vm.stop();
    return 0;
}

int cmd_stats(const Args& a) {
    std::string note;
    auto p = make_provider(a, note);
    VisualMemory vm(make_cfg(a, p != nullptr));
    if (p) vm.set_embedding_provider(p);
    if (!vm.start()) { std::fprintf(stderr, "start failed\n"); return 2; }
    print_stats(vm);
    vm.stop();
    return 0;
}

// Synthetic-vector scaling benchmark: fills the vector table directly (via the public API only,
// i.e. through real frames + the mock provider) is too slow for 10k, so this uses the mock provider
// with tiny JPEGs. Reports insert and query latency per database size.
int cmd_bench_db(const Args& a) {
    const int dim = static_cast<int>(a.num("dim", 1280));
    std::vector<size_t> sizes;
    { std::stringstream ss(a.get("sizes", "100,1000,10000")); std::string t;
      while (std::getline(ss, t, ',')) sizes.push_back(std::strtoull(t.c_str(), nullptr, 10)); }
    // A dedicated tiny 8x8 JPEG per frame is produced on the fly with TurboJPEG.
    auto make_jpeg = [](uint32_t seed) {
        std::vector<uint8_t> px(8 * 8 * 3);
        std::mt19937 g(seed);
        for (auto& b : px) b = static_cast<uint8_t>(g());
        tjhandle tj = tjInitCompress(); unsigned char* out = nullptr; unsigned long n = 0;
        tjCompress2(tj, px.data(), 8, 0, 8, TJPF_RGB, &out, &n, TJSAMP_444, 90, 0);
        std::vector<uint8_t> v(out, out + n); tjFree(out); tjDestroy(tj); return v; };
    auto mock = std::make_shared<MockEmbeddingProvider>(dim);
    Config c = make_cfg(a, true);
    c.embedding.max_pending = 4096; c.max_storage_bytes = 0; c.retention_hours = 0; c.embedding.retention_days = 0;
    c.max_pending_frames = 4096;
    VisualMemory vm(c);
    vm.set_embedding_provider(mock);
    if (!vm.start()) { std::fprintf(stderr, "start failed\n"); return 2; }
    std::printf("backend=%s dim=%d\n", vm.embedding_stats().backend.c_str(), dim);
    size_t have = 0; uint64_t seq = 0;
    std::mt19937 rng(7);
    for (size_t target : sizes) {
        auto t0 = std::chrono::steady_clock::now();
        while (have < target) {
            OwnedFrame f; f.width = 8; f.height = 8; f.pixel_format = PixelFormat::JPEG; f.pixels = make_jpeg(uint32_t(seq));
            f.metadata.source_id = (seq % 2) ? "camA" : "camB"; f.metadata.source_type = "file"; f.metadata.device_id = "bench";
            f.metadata.capture_utc_ms = 1767261600000LL + int64_t(seq) * 5000; f.metadata.source_sequence = ++seq;
            while (!vm.try_submit(OwnedFrame(f))) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++have;
            if (have % 256 == 0) while (vm.stats().encoded + 512 < have) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        while (vm.stats().encoded < have) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (int guard = 0; guard < 600 && vm.embedding_stats().vectors_stored < have; ++guard) {
            vm.backfill_embeddings(have, 0);                 // picks up anything the bounded queue dropped
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (vm.embedding_stats().vectors_stored < have) { std::printf("  WARNING: only %llu of %zu vectors stored\n", (unsigned long long)vm.embedding_stats().vectors_stored, have); }
        const double fill_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        // queries: full scan, time-window (10%), source filter
        std::vector<float> q(dim); for (auto& x : q) x = float(rng() % 2001) / 1000.f - 1.f;
        auto run = [&](const char* label, SearchOptions o) {
            std::vector<double> t;
            for (int i = 0; i < 20; ++i) {
                SearchStatus st; auto s0 = std::chrono::steady_clock::now();
                vm.search_similar(q, o, &st);
                t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count());
                if (!st.ok()) { std::printf("  %s: ERROR %s\n", label, st.message.c_str()); return; }
            }
            std::sort(t.begin(), t.end());
            std::printf("  N=%-7zu %-22s p50=%8.2f p95=%8.2f p99=%8.2f max=%8.2f ms\n", have, label, t[9], t[18], t[19], t[19]);
        };
        std::error_code ec;
        const auto dbsz = fs::file_size(fs::path(a.get("root")) / "visual_memory.db", ec);
        std::printf("size %zu reached in %.1fs | visual_memory.db = %.2f MiB (%.0f B/vector incl. frames metadata + vec0 chunks)\n",
                    have, fill_s, dbsz / 1048576.0, have ? double(dbsz) / have : 0.0);
        SearchOptions o; o.top_k = 5;
        run("top5 full scan", o);
        SearchOptions w = o; w.start_utc_ms = 1767261600000LL; w.end_utc_ms = 1767261600000LL + int64_t(have / 10) * 5000; run("top5 10% time window", w);
        SearchOptions s = o; s.source_id = "camA"; run("top5 source filter", s);
    }
    print_stats(vm);
    vm.stop();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Args a = parse(argc, argv);
    if (a.cmd == "embed") return cmd_embed(a);
    if (a.cmd == "ingest") return cmd_ingest(a);
    if (a.cmd == "search") return cmd_search(a);
    if (a.cmd == "backfill") return cmd_backfill(a);
    if (a.cmd == "stats") return cmd_stats(a);
    if (a.cmd == "bench-db") return cmd_bench_db(a);
    std::fprintf(stderr, "usage: vm_embed_demo embed|ingest|search|backfill|stats|bench-db [--key value ...]\n");
    return 1;
}
