// SPDX-License-Identifier: MIT
//
// Minimal, Argus-free demonstration of libvisualmemory: generate a handful of
// synthetic BGR frames, persist them as JPEGs + SQLite rows, then query back
// and print the results. Deterministic and runnable on any Linux host.
//
//   ./vm_ingest_image [output_dir]
//
#include "visualmemory/visual_memory.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace visualmemory;

static OwnedFrame synth_frame(int w, int h, const std::string& src, int64_t ts, uint64_t seq) {
    OwnedFrame f;
    f.width = w;
    f.height = h;
    f.pixel_format = PixelFormat::BGR24;
    f.stride0 = w * 3;
    f.pixels.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = f.pixels.data() + (static_cast<size_t>(y) * w + x) * 3;
            p[0] = static_cast<uint8_t>((x + seq) % 256);
            p[1] = static_cast<uint8_t>(y % 256);
            p[2] = static_cast<uint8_t>((x ^ y) % 256);
        }
    f.metadata.source_id = src;
    f.metadata.source_type = "file";
    f.metadata.device_id = "example-device";
    f.metadata.capture_utc_ms = ts;
    f.metadata.source_sequence = seq;
    return f;
}

int main(int argc, char** argv) {
    std::string root = (argc > 1) ? argv[1]
                                  : (std::filesystem::temp_directory_path() / "vm_example").string();

    Config cfg;
    cfg.root_directory = root;
    cfg.stored_width = 640;
    cfg.stored_height = 360;
    cfg.jpeg_quality = 80;

    VisualMemory vm(cfg);
    if (!vm.start()) {
        std::fprintf(stderr, "failed to start VisualMemory at %s\n", root.c_str());
        return 1;
    }
    std::printf("storage root: %s\n", root.c_str());

    const int64_t base = 1'700'000'000'000;  // fixed epoch ms for determinism
    for (int i = 0; i < 5; ++i) {
        auto f = synth_frame(1280, 720, "file:demo.mp4", base + i * 5000, i + 1);
        if (!vm.try_submit(std::move(f)))
            std::fprintf(stderr, "submit %d dropped\n", i);
    }

    // Wait for the worker to persist everything.
    for (int i = 0; i < 500 && vm.stats().encoded < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto rows = vm.query_by_time(base, base + 100000, "", 0, 0);
    std::printf("persisted %zu frames (encoded=%llu, dropped=%llu)\n", rows.size(),
                (unsigned long long)vm.stats().encoded,
                (unsigned long long)vm.stats().dropped_queue_full);
    for (const auto& r : rows)
        std::printf("  id=%lld  %s  %dx%d  %llu bytes  %s\n", (long long)r.id,
                    r.relative_jpeg_path.c_str(), r.width, r.height,
                    (unsigned long long)r.file_size_bytes, r.sha256_hex.c_str());

    vm.stop();
    return rows.size() == 5 ? 0 : 2;
}
