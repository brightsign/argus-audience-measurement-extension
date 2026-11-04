// vm_ingest_dir - developer/demo tool: ingest a list of JPEG files into a Visual Memory store through the real
// libvisualmemory writer (Stage 1) and the real RKNN embedder (Stage 2), with explicit capture timestamps.
//
//   vm_ingest_dir --root DIR --model PATH [--npu-core N] [--span-minutes 120] [--source-id ID] [--append] a.jpg b.jpg ...
//
// Files are stored in the order given; capture times are spread evenly over the last --span-minutes ending now, so the
// date filters of the dashboard have something to filter. Not shipped on the player by default (built as a dev target,
// copy it next to the extension binaries or to /tmp to run). Never writes to the active Argus store unless pointed at it.
#include "visualmemory/visual_memory.h"
#ifdef ARGUS_VISUAL_EMBEDDINGS
#include "rknn_embedding_provider.h"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace visualmemory;
namespace fs = std::filesystem;

static bool jpeg_size(const std::vector<uint8_t>& d, int& w, int& h) {       // parse the SOFn marker
    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    size_t i = 2;
    while (i + 9 < d.size()) {
        if (d[i] != 0xFF) { ++i; continue; }
        const uint8_t m = d[i + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        const size_t len = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
        if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            h = (d[i + 5] << 8) | d[i + 6]; w = (d[i + 7] << 8) | d[i + 8]; return w > 0 && h > 0;
        }
        i += 2 + len;
    }
    return false;
}

int main(int argc, char** argv) {
    std::string root, model, source = "demo-scenes";
    int core = -1, span_min = 120; bool append = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--root") root = next();
        else if (a == "--model") model = next();
        else if (a == "--npu-core") core = std::atoi(next().c_str());
        else if (a == "--span-minutes") span_min = std::max(1, std::atoi(next().c_str()));
        else if (a == "--source-id") source = next();
        else if (a == "--append") append = true;
        else files.push_back(a);
    }
    if (root.empty() || files.empty()) {
        std::fprintf(stderr, "usage: %s --root DIR --model PATH [--npu-core N] [--span-minutes M] [--source-id ID] [--append] files...\n", argv[0]);
        return 2;
    }
    if (!append && fs::exists(fs::path(root) / "visual_memory.db")) {
        std::fprintf(stderr, "refusing: %s already holds a store (use --append to add to it)\n", root.c_str());
        return 3;
    }
    Config cfg;
    cfg.root_directory = root;
    cfg.stored_width = 640; cfg.stored_height = 360; cfg.jpeg_quality = 90;
    cfg.max_storage_bytes = 4ULL * 1000 * 1000 * 1000;
    cfg.retention_hours = 48;
    cfg.embedding.enabled = !model.empty();
    VisualMemory vm(cfg);
#ifdef ARGUS_VISUAL_EMBEDDINGS
    if (!model.empty()) {
        RknnEmbeddingProvider::Options ro; ro.model_path = model; ro.npu_core = core;
        auto p = std::make_shared<RknnEmbeddingProvider>(ro);
        if (!p->init()) { std::fprintf(stderr, "embedding model failed to load: %s\n", p->last_error().c_str()); return 4; }
        vm.set_embedding_provider(p);
    }
#endif
    if (!vm.start()) { std::fprintf(stderr, "cannot start Visual Memory at %s\n", root.c_str()); return 5; }

    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t span_ms = static_cast<int64_t>(span_min) * 60000;
    const int64_t step = files.size() > 1 ? span_ms / static_cast<int64_t>(files.size() - 1) : 0;
    const int64_t t0 = now - span_ms;
    size_t ok = 0;
    for (size_t k = 0; k < files.size(); ++k) {
        std::ifstream in(files[k], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        OwnedFrame f; int w = 0, h = 0;
        if (!jpeg_size(bytes, w, h)) { std::fprintf(stderr, "skip (not a JPEG): %s\n", files[k].c_str()); continue; }
        f.width = w; f.height = h; f.pixel_format = PixelFormat::JPEG; f.pixels = std::move(bytes);
        f.metadata.source_id = source; f.metadata.source_type = "file";
        f.metadata.device_id = "demo-dataset"; f.metadata.capture_utc_ms = t0 + step * static_cast<int64_t>(k);
        f.metadata.source_sequence = k + 1;
        const uint64_t before = vm.stats().encoded;
        if (!vm.try_submit(std::move(f))) { std::fprintf(stderr, "dropped: %s\n", files[k].c_str()); continue; }
        for (int i = 0; i < 3000 && vm.stats().encoded <= before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (cfg.embedding.enabled)   // keep the bounded Stage-2 queue from overflowing: one frame in flight
            for (int i = 0; i < 3000 && vm.embedding_stats().queue_depth > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ++ok;
    }
    for (int i = 0; i < 6000 && cfg.embedding.enabled &&
                    vm.embedding_stats().embedded + vm.embedding_stats().failed_permanent < ok; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto s = vm.stats();
    std::printf("ingested %zu/%zu frames into %s | encoded=%llu dropped=%llu write_fail=%llu db_fail=%llu", ok, files.size(),
                root.c_str(), (unsigned long long)s.encoded, (unsigned long long)s.dropped_queue_full,
                (unsigned long long)s.jpeg_write_failures, (unsigned long long)s.database_failures);
    if (cfg.embedding.enabled) {
        const auto e = vm.embedding_stats();
        std::printf(" | embedded=%llu failed=%llu", (unsigned long long)e.embedded, (unsigned long long)e.failed_permanent);
    }
    std::printf("\n");
    vm.stop();
    return ok == files.size() ? 0 : 6;
}
