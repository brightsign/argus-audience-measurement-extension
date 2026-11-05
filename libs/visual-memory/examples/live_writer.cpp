// SPDX-License-Identifier: MIT
//
// Test helper: behaves like the Argus writer process. Opens a store, then submits one synthetic (people-free) frame
// every INTERVAL_MS with capture time = now, keeping the store open between frames, so a reader process (vm_api_server)
// sees rows appear while it is running. Used by tests/browser/vm_gallery_live_cdp.py.
//
//   vm_live_writer ROOT COUNT INTERVAL_MS [SOURCE_ID]
//
#include "visualmemory/visual_memory.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace visualmemory;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: vm_live_writer ROOT COUNT INTERVAL_MS [SOURCE_ID]\n"); return 2; }
    Config cfg;
    cfg.root_directory = argv[1];
    cfg.stored_width = 640; cfg.stored_height = 360; cfg.jpeg_quality = 75;
    cfg.retention_hours = 0; cfg.max_storage_bytes = 0;        // never prune a test store
    const int count = std::atoi(argv[2]);
    const int interval = std::atoi(argv[3]);
    const std::string src = argc > 4 ? argv[4] : "/dev/video-test";
    VisualMemory vm(cfg);
    if (!vm.start()) { std::fprintf(stderr, "cannot start store at %s\n", cfg.root_directory.c_str()); return 1; }
    for (int i = 0; i < count; ++i) {
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        OwnedFrame f;
        f.width = 640; f.height = 480; f.pixel_format = PixelFormat::BGR24; f.stride0 = f.width * 3;
        f.pixels.resize(static_cast<size_t>(f.width) * f.height * 3);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) {
                uint8_t* p = f.pixels.data() + (static_cast<size_t>(y) * f.width + x) * 3;
                p[0] = static_cast<uint8_t>((x + i * 17) % 256); p[1] = static_cast<uint8_t>((y * 2 + i * 5) % 256); p[2] = static_cast<uint8_t>(40 + (i * 31) % 200);
            }
        f.metadata.source_id = src; f.metadata.source_type = "usb"; f.metadata.device_id = "live-writer-test";
        f.metadata.capture_utc_ms = now; f.metadata.source_sequence = static_cast<uint64_t>(i + 1);
        if (!vm.try_submit(std::move(f))) std::fprintf(stderr, "frame %d dropped\n", i);
        std::printf("frame %d submitted at %lld\n", i, static_cast<long long>(now)); std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
    }
    for (int i = 0; i < 200 && vm.stats().encoded < vm.stats().accepted; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    vm.stop();
    return 0;
}
