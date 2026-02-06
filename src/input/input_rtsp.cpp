#include "input/input_rtsp.h"
#include "metrics/log_global.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <mutex>
#include <memory>
#include <cstring>
#include <cstdlib>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>

#include <rga/rga.h>
#include <rga/im2d.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// ===============================
// Network Wait Helpers
// ===============================
static bool any_interface_has_ip(std::string &iface_out, std::string &ip_out) {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return false;
    bool found = false;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            struct sockaddr_in *sa = (struct sockaddr_in*)ifa->ifa_addr;
            char ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip))) {
                if (strcmp(ip, "0.0.0.0") != 0) {
                    iface_out = ifa->ifa_name;
                    ip_out    = ip;
                    found = true;
                    break;
                }
            }
        }
    }
    freeifaddrs(ifaddr);
    return found;
}

static void wait_for_network_with_validation(int retries = 30, int delay_sec = 2) {
    LG_INFO("input_rtsp:waiting for network connectivity...\n");
    for (int i = 0; i < retries; i++) {
        std::string iface, ip;
        if (any_interface_has_ip(iface, ip)) {
            LG_INFO("input_rtsp:network interface %s has IP %s\n", iface.c_str(), ip.c_str());
            LG_INFO("input_rtsp:network connectivity established\n");
            return;
        }
        LG_WARN("input_rtsp:waiting for network (attempt %d/%d)...\n", i+1, retries);
        std::this_thread::sleep_for(std::chrono::seconds(delay_sec));
    }
    LG_ERROR("input_rtsp:no network interface got an IP after %d attempts\n", retries);
}

// ===============================
// GStreamer One-time Init
// ===============================
static bool check_gstreamer_plugins() {
    static bool checked = false;
    static bool all_present = false;
    
    if (checked) return all_present;
    checked = true;
    
    LG_INFO("input_rtsp:checking GStreamer plugin availability...\n");
    LG_INFO("input_rtsp:GST_PLUGIN_PATH=%s\n", getenv("GST_PLUGIN_PATH") ? getenv("GST_PLUGIN_PATH") : "(not set)");
    LG_INFO("input_rtsp:LD_LIBRARY_PATH=%s\n", getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "(not set)");
    
    auto have = [](const char* name) -> bool {
        GstElementFactory* factory = gst_element_factory_find(name);
        bool found = (factory != nullptr);
        if (factory) gst_object_unref(factory);
        return found;
    };
    
    // CRITICAL plugins - RTSP will NOT work without these
    struct { const char* name; const char* plugin_file; bool critical; } plugins[] = {
        {"rtspsrc",      "libgstrtsp.so (gst-plugins-good)",         true},
        {"appsink",      "libgstapp.so (gst-plugins-base)",          true},
        {"rtph264depay", "libgstrtp.so (gst-plugins-good)",          true},
        {"h264parse",    "libgstvideoparsersbad.so (gst-plugins-bad)", true},
        {"queue",        "libgstcoreelements.so (gst-plugins-base)", true},
        {"mppvideodec",  "libgstrockchipmpp.so (platform)",          false}, // or avdec_h264
        {"avdec_h264",   "libgstlibav.so (gst-libav)",               false},
        {"videoconvert", "libgstvideoconvertscale.so (gst-plugins-base)", false},
        {"decodebin",    "libgstplayback.so (gst-plugins-base)",     false}
    };
    
    all_present = true;
    bool has_decoder = false;
    
    for (const auto& p : plugins) {
        bool found = have(p.name);
        if (found) {
            LG_INFO("input_rtsp:  ✓ %s available (%s)\n", p.name, p.plugin_file);
            if (strcmp(p.name, "mppvideodec") == 0 || strcmp(p.name, "avdec_h264") == 0) {
                has_decoder = true;
            }
        } else {
            if (p.critical) {
                LG_ERROR("input_rtsp:  ✗ %s NOT FOUND - REQUIRED (%s)\n", p.name, p.plugin_file);
                all_present = false;
            } else {
                LG_WARN("input_rtsp:  ✗ %s NOT FOUND - optional (%s)\n", p.name, p.plugin_file);
            }
        }
    }
    
    if (!has_decoder) {
        LG_ERROR("input_rtsp:  ✗ No H264 decoder available (need mppvideodec OR avdec_h264)\n");
        all_present = false;
    }
    
    if (!all_present) {
        LG_ERROR("input_rtsp:===================================================================\n");
        LG_ERROR("input_rtsp:CRITICAL PLUGINS MISSING - RTSP WILL NOT WORK\n");
        LG_ERROR("input_rtsp:===================================================================\n");
        LG_ERROR("input_rtsp:Required GStreamer plugins are not installed or not in plugin path.\n");
        LG_ERROR("input_rtsp:\n");
        LG_ERROR("input_rtsp:Verify on device:\n");
        LG_ERROR("input_rtsp:  1. Check bundled plugins exist:\n");
        LG_ERROR("input_rtsp:     ls -la /var/volatile/bsext/ext_npu_argus/RK3568/lib/gstreamer-1.0/\n");
        LG_ERROR("input_rtsp:  2. Test plugin loading:\n");
        LG_ERROR("input_rtsp:     export GST_PLUGIN_PATH=/var/volatile/bsext/ext_npu_argus/RK3568/lib/gstreamer-1.0:/usr/lib/gstreamer-1.0\n");
        LG_ERROR("input_rtsp:     export LD_LIBRARY_PATH=/var/volatile/bsext/ext_npu_argus/RK3568/lib:$LD_LIBRARY_PATH\n");
        LG_ERROR("input_rtsp:     gst-inspect-1.0 rtspsrc\n");
        LG_ERROR("input_rtsp:     gst-inspect-1.0 appsink\n");
        LG_ERROR("input_rtsp:  3. Check for missing dependencies:\n");
        LG_ERROR("input_rtsp:     ldd /var/volatile/bsext/ext_npu_argus/RK3568/lib/gstreamer-1.0/libgstrtsp.so\n");
        LG_ERROR("input_rtsp:     ldd /var/volatile/bsext/ext_npu_argus/RK3568/lib/gstreamer-1.0/libgstapp.so\n");
        LG_ERROR("input_rtsp:===================================================================\n");
    } else {
        LG_INFO("input_rtsp:✅ All critical GStreamer plugins are available\n");
    }
    
    return all_present;
}

static void gst_init_once() {
    static std::once_flag f;
    std::call_once(f, []{
        // Delete stale registry to force plugin rescan
        unlink("/tmp/gst-registry.bin");
        
        int argc = 0; char** argv = nullptr;
        gst_init(&argc, &argv);
        
        if (!getenv("GST_REGISTRY")) setenv("GST_REGISTRY", "/tmp/gst-registry.bin", 1);
    });
}

static void ensure_gstreamer_runtime() {
    // GST_PLUGIN_PATH is already set by bsext_init wrapper script
    // This function just logs the current configuration
    const char* path = getenv("GST_PLUGIN_PATH");
    LG_INFO("input_rtsp:GST_PLUGIN_PATH=%s\n", path ? path : "(not set - will use defaults)");
    
    if (!getenv("GST_REGISTRY"))
        setenv("GST_REGISTRY", "/tmp/gst-registry.bin", 1);
}

// ===============================
// Build RTSP GStreamer Pipelines
// ===============================
static std::vector<std::string> build_rtsp_pipelines(const std::string& url_in) {
    auto make_url_variants = [&](const std::string& u) {
        std::vector<std::string> urls;
        urls.push_back(u); // original URL first (e.g., /stream1)
        
        // Try /stream2 as fallback if /stream1 was specified (Tapo cameras often have substream)
        // But prioritize what the user explicitly configured
        try {
            auto pos = u.rfind("/stream1");
            if (pos != std::string::npos) {
                std::string u2 = u;
                u2.replace(pos, 8, "/stream2");
                urls.push_back(u2);  // Add as fallback, not first choice
            }
        } catch (...) {}
        return urls;
    };

    auto mk = [&](const std::string& url, const char* proto, const char* enc, bool hw_decode, bool scaled_320, bool hw_scale) {
        std::string s =
            "rtspsrc location=" + url +
            " protocols=" + proto +
            // Phase 2B: Reduced latency from 200ms to 50ms for low-latency operation
            // On local network RTSP cameras, 200ms is excessive - 50ms provides adequate
            // buffer while minimizing end-to-end latency (~1ms gain per frame)
            " latency=50 drop-on-latency=true do-rtsp-keep-alive=true "
            " tcp-timeout=10000000000 timeout=15000000000 retry=3 "
            " name=src ";
        s += "src. ! ";

        if (enc) {
            s += std::string("application/x-rtp,media=video,encoding-name=") + enc + " ! ";
            if (std::string(enc) == "H265") {
                s += "rtph265depay ! h265parse disable-passthrough=true ! ";
            } else {
                s += "rtph264depay ! h264parse config-interval=1 disable-passthrough=true ! ";
            }
            if (hw_decode) {
                s += "mppvideodec ! ";
            } else {
                s += (std::string(enc) == "H265" ? "avdec_hevc ! " : "avdec_h264 ! ");
                s += "videoconvert ! ";
            }
        } else {
            // Auto-detect codec with decodebin
            // Try multiple color conversion options for BrightSign compatibility
            s += "decodebin ! videoconvert ! video/x-raw,format=NV12 ! ";
        }

        if (scaled_320) {
            if (hw_scale) s += "rkvideoscale ! ";
            else          s += "videoscale ! ";
            s += "video/x-raw,format=NV12,width=320,height=320 ! ";
        }

        s += "video/x-raw,format=NV12 ! "
             // Phase 2B: Keep queue small (max-size-buffers=2) for minimal latency
             // leaky=downstream ensures we drop old frames under load (correct for real-time)
             "queue max-size-buffers=2 leaky=downstream ! "
             // Phase 2B: Increased max-buffers from 1 to 2 (~0.5ms gain)
             // Allows pipeline buffering: one frame processing, one queued
             // Still drops old frames (drop=1) to prevent stale data
             "appsink name=mysink caps=video/x-raw,format=NV12 "
             "drop=1 max-buffers=2 enable-last-sample=false sync=false";
        return s;
    };

    std::vector<std::string> p;
    auto urls = make_url_variants(url_in);

    for (const auto& url : urls) {
        // Try hardware decode FIRST (mppvideodec is native on LS5/Rockchip - no videoconvert needed)
        p.push_back(mk(url, "tcp", "H264", /*hw_decode=*/true,  /*scaled_320=*/false, /*hw_scale=*/false));
        
        // Try H265 hardware decode in case camera is using H.265
        p.push_back(mk(url, "tcp", "H265", /*hw_decode=*/true,  /*scaled_320=*/false, /*hw_scale=*/false));
        
        // Try software decode as fallback (needs videoconvert)
        p.push_back(mk(url, "tcp", "H264", /*hw_decode=*/false, /*scaled_320=*/false, /*hw_scale=*/false));
        
        // Try auto-detect with decodebin
        p.push_back(mk(url, "tcp", nullptr,/*hw_decode=*/false,  /*scaled_320=*/false, /*hw_scale=*/false));
        
        // UDP fallbacks
        p.push_back(mk(url, "udp", "H264", /*hw_decode=*/true, /*scaled_320=*/false, /*hw_scale=*/false));
        p.push_back(mk(url, "udp", "H264", /*hw_decode=*/false, /*scaled_320=*/false, /*hw_scale=*/false));
    }
    return p;
}

// ===============================
// RtspNv12 Helper (GStreamer Wrapper)
// ===============================
class RtspNv12Helper {
public:
    ~RtspNv12Helper() { close(); }

    bool open(const std::string& url, int first_frame_timeout_ms = 10000) {
        close();
        gst_init_once();
        ensure_gstreamer_runtime();
        
        // CRITICAL: check_gstreamer_plugins() MUST be called AFTER gst_init_once()
        // because GStreamer needs to be initialized before querying plugin factories
        // Returns false if critical plugins are missing - fail fast with clear error
        if (!check_gstreamer_plugins()) {
            LG_ERROR("input_rtsp:cannot open RTSP stream - critical GStreamer plugins missing\n");
            LG_ERROR("input_rtsp:this is a packaging/deployment issue, not a camera issue\n");
            return false;
        }

        auto pipelines = build_rtsp_pipelines(url);
        GError* err = nullptr;

        for (const auto& pipeline_str : pipelines) {
            LG_INFO("input_rtsp:trying pipeline (full): %s\n", pipeline_str.c_str());

            // Add delay between attempts to avoid overwhelming camera/network
            // Tapo cameras can be sensitive to rapid connection attempts
            static auto last_attempt = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_attempt).count();
            if (elapsed_ms < 500) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500 - elapsed_ms));
            }
            last_attempt = std::chrono::steady_clock::now();

            err = nullptr;
            pipeline_ = gst_parse_launch(pipeline_str.c_str(), &err);
            if (!pipeline_) {
                LG_ERROR("input_rtsp:gst_parse_launch failed: %s\n", err ? err->message : "(unknown)");
                if (err) { g_error_free(err); err = nullptr; }
                continue;
            }
            
            if (err) {
                LG_WARN("input_rtsp:gst_parse_launch warning: %s\n", err->message);
                g_error_free(err);
                err = nullptr;
            }
            
            LG_INFO("input_rtsp:pipeline created successfully, looking for appsink 'mysink'\n");

            appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mysink");
            if (!appsink_) {
                LG_ERROR("input_rtsp:appsink 'mysink' not found in pipeline\n");
                LG_ERROR("input_rtsp:this means pipeline elements failed to link - checking for missing plugins\n");
                
                // Try to get pipeline state to see if there are errors
                GstState state, pending;
                GstStateChangeReturn ret = gst_element_get_state(pipeline_, &state, &pending, 0);
                LG_ERROR("input_rtsp:pipeline state: %d, pending: %d, return: %d\n", state, pending, ret);
                
                gst_object_unref(pipeline_);
                pipeline_ = nullptr;
                continue;
            }

            GstCaps* caps = gst_caps_from_string("video/x-raw,format=NV12");
            // Phase 2B: Optimized appsink configuration for low latency
            // max-buffers=2: Allows pipelining (1 processing, 1 queued) vs blocking on single buffer
            // drop=1: Always drop oldest frame on overflow (prevents stale data)
            // sync=false: Don't sync to clock (real-time capture, not playback)
            // enable-last-sample=false: Don't keep last sample in memory (saves RAM)
            g_object_set(G_OBJECT(appsink_),
                        "caps", caps,
                        "drop", 1,
                        "max-buffers", 2,  // Phase 2B: Increased from 1 to 2
                        "enable-last-sample", FALSE,
                        "sync", FALSE,
                        nullptr);
            gst_caps_unref(caps);
            
            LG_INFO("input_rtsp:appsink configured with max-buffers=2 (Phase 2B optimization)\n");

            bus_ = gst_element_get_bus(pipeline_);
            broken_.store(false, std::memory_order_release);
            bus_running_.store(true, std::memory_order_release);

            // Bus watcher thread
            bus_thread_ = std::thread([this]{
                while (bus_running_.load(std::memory_order_acquire)) {
                    GstMessage* msg = gst_bus_timed_pop_filtered(
                        bus_, 300 * GST_MSECOND,
                        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_WARNING));
                    if (!msg) continue;

                    switch (GST_MESSAGE_TYPE(msg)) {
                        case GST_MESSAGE_ERROR: {
                            GError* e=nullptr; gchar* dbg=nullptr;
                            gst_message_parse_error(msg, &e, &dbg);
                            LG_ERROR("input_rtsp:GST_MESSAGE_ERROR: %s\n", e ? e->message : "(unknown)");
                            if (dbg) g_free(dbg);
                            if (e) g_error_free(e);
                            broken_.store(true, std::memory_order_release);
                            break;
                        }
                        case GST_MESSAGE_EOS:
                            LG_WARN("input_rtsp:GST_MESSAGE_EOS received\n");
                            broken_.store(true, std::memory_order_release);
                            break;
                        case GST_MESSAGE_WARNING: {
                            GError* e=nullptr; gchar* dbg=nullptr;
                            gst_message_parse_warning(msg, &e, &dbg);
                            LG_WARN("input_rtsp:GST_MESSAGE_WARNING: %s\n", e ? e->message : "(unknown)");
                            if (dbg) g_free(dbg);
                            if (e) g_error_free(e);
                            break;
                        }
                        default: break;
                    }
                    gst_message_unref(msg);
                }
            });

            gst_element_set_state(pipeline_, GST_STATE_PLAYING);

            // Wait for first frame (prove connectivity)
            LG_INFO("input_rtsp:waiting up to %d ms for first frame...\n", first_frame_timeout_ms);
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), first_frame_timeout_ms * GST_MSECOND);
            if (!sample) {
                LG_ERROR("input_rtsp:first-frame timeout (%d ms) on this pipeline - no frame received\n", first_frame_timeout_ms);
                LG_ERROR("input_rtsp:pipeline may have errors. Check if camera is streaming and credentials are correct.\n");
                // Properly cleanup before trying next pipeline
                bus_running_.store(false, std::memory_order_release);
                if (bus_thread_.joinable()) bus_thread_.join();
                gst_element_set_state(pipeline_, GST_STATE_NULL);
                gst_object_unref(appsink_); appsink_ = nullptr;
                gst_object_unref(bus_); bus_ = nullptr;
                gst_object_unref(pipeline_); pipeline_ = nullptr;
                continue;
            }

            GstCaps* scaps = gst_sample_get_caps(sample);
            const GstStructure* s = gst_caps_get_structure(scaps, 0);
            gst_structure_get_int(s, "width", &w_);
            gst_structure_get_int(s, "height", &h_);
            gst_sample_unref(sample);

            nv12_small_.assign(320 * 320 * 3 / 2, 0);
            rgb_scratch_.assign(320 * 320 * 3, 0);

            LG_INFO("input_rtsp:RTSP NV12 stream opened: %dx%d\n", w_, h_);
            return true;
        }

        if (err) g_error_free(err);
        LG_ERROR("input_rtsp:all RTSP pipeline attempts failed\n");
        return false;
    }

    bool pull_into_rgb(uint8_t* rgb_out_320x320) {
        if (!pipeline_ || !appsink_) return false;
        if (broken_.load(std::memory_order_acquire)) return false;

        const int per_try_ms = 200, tries = 5;
        if (!rgb_out_320x320) return false;

        for (int i = 0; i < tries; ++i) {
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), per_try_ms * GST_MSECOND);
            if (!sample) continue;

            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstCaps*   caps   = gst_sample_get_caps(sample);
            if (!buffer || !caps) { gst_sample_unref(sample); continue; }

            GstVideoInfo vinfo;
            if (!gst_video_info_from_caps(&vinfo, caps)) { gst_sample_unref(sample); continue; }

            GstVideoFrame vframe;
            if (!gst_video_frame_map(&vframe, &vinfo, buffer, GST_MAP_READ)) { gst_sample_unref(sample); continue; }

            const int W = GST_VIDEO_INFO_WIDTH(&vinfo);
            const int H = GST_VIDEO_INFO_HEIGHT(&vinfo);

            uint8_t* y_ptr     = (uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0);
            int      y_stride  = (int)GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0);
            uint8_t* uv_ptr    = (uint8_t*)GST_VIDEO_FRAME_PLANE_DATA(&vframe, 1);
            int      uv_stride = (int)GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 1);

            static std::vector<uint8_t> nv12_full;
            bool stride_ok = (y_stride == W) && (uv_stride == W);
            bool contiguous = stride_ok && (uv_ptr == y_ptr + (size_t)y_stride * H);

            const uint8_t* src_base = nullptr;
            if (contiguous) {
                src_base = y_ptr;
            } else {
                nv12_full.resize(W*H*3/2);
                for (int r=0; r<H; ++r)
                    memcpy(&nv12_full[r*W], y_ptr + r*y_stride, W);
                uint8_t* uv_dst = nv12_full.data() + W*H;
                for (int r=0; r<H/2; ++r)
                    memcpy(uv_dst + r*W, uv_ptr + r*uv_stride, W);
                src_base = nv12_full.data();
            }

            bool ok = false;
            do {
                // Try RGA first (hardware acceleration)
                // NV12 (W×H) -> NV12 (320×320) via RGA
                rga_buffer_t src_nv12       = wrapbuffer_virtualaddr((void*)src_base, W, H, RK_FORMAT_YCbCr_420_SP);
                rga_buffer_t dst_nv12_small = wrapbuffer_virtualaddr(nv12_small_.data(), 320, 320, RK_FORMAT_YCbCr_420_SP);
                double fx = 320.0 / (double)W;
                double fy = 320.0 / (double)H;
                int ret = imresize(src_nv12, dst_nv12_small, fx, fy, 0, IM_SYNC);
                
                if (ret == IM_STATUS_SUCCESS) {
                    // NV12 (320×320) -> BGR (320×320) via RGA
                    rga_buffer_t src_small = wrapbuffer_virtualaddr(nv12_small_.data(), 320, 320, RK_FORMAT_YCbCr_420_SP);
                    rga_buffer_t dst_bgr   = wrapbuffer_virtualaddr(rgb_out_320x320, 320, 320, RK_FORMAT_BGR_888);
                    ret = imcvtcolor(src_small, dst_bgr, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888, IM_SYNC);
                    
                    if (ret == IM_STATUS_SUCCESS) {
                        ok = true;
                        break;  // RGA succeeded
                    }
                }
                
                // RGA failed, fall back to OpenCV (software processing for RK3576)
                static bool logged_fallback = false;
                if (!logged_fallback) {
                    LG_WARN("input_rtsp:RGA failed, using OpenCV for NV12->BGR conversion (slower)\n");
                    logged_fallback = true;
                }
                
                // OpenCV NV12 -> BGR conversion
                // NV12 has Y plane (W×H) followed by interleaved UV plane (W×H/2)
                cv::Mat nv12_mat(H + H/2, W, CV_8UC1, (void*)src_base);
                cv::Mat bgr_full(H, W, CV_8UC3);
                cv::cvtColor(nv12_mat, bgr_full, cv::COLOR_YUV2BGR_NV12);
                
                // Resize to 320×320
                cv::Mat bgr_small(320, 320, CV_8UC3, rgb_out_320x320);
                cv::resize(bgr_full, bgr_small, cv::Size(320, 320), 0, 0, cv::INTER_LINEAR);
                
                ok = true;
            } while(false);

            gst_video_frame_unmap(&vframe);
            gst_sample_unref(sample);
            return ok;
        }
        return false;
    }

    bool broken() const { return broken_.load(std::memory_order_acquire); }
    int  width()  const { return w_; }
    int  height() const { return h_; }

    void close() {
        bus_running_.store(false, std::memory_order_release);
        if (bus_thread_.joinable()) bus_thread_.join();

        if (pipeline_) { gst_element_set_state(pipeline_, GST_STATE_NULL); gst_object_unref(pipeline_); pipeline_ = nullptr; }
        if (appsink_)  { gst_object_unref(appsink_); appsink_ = nullptr; }
        if (bus_)      { gst_object_unref(bus_); bus_ = nullptr; }

        w_ = h_ = 0;
        nv12_small_.clear(); nv12_small_.shrink_to_fit();
        rgb_scratch_.clear(); rgb_scratch_.shrink_to_fit();
        broken_.store(false, std::memory_order_release);
    }

private:
    GstElement* pipeline_ = nullptr;
    GstElement* appsink_  = nullptr;
    GstBus*     bus_      = nullptr;

    int w_ = 0, h_ = 0;

    std::vector<uint8_t> nv12_small_;
    std::vector<uint8_t> rgb_scratch_;

    std::thread bus_thread_;
    std::atomic<bool> bus_running_{false};
    std::atomic<bool> broken_{false};
};

// ===============================
// RtspInputSource::Impl
// ===============================
struct RtspInputSource::Impl {
  std::string url;
  RtspOptions opts;
  std::atomic<bool> opened{false};
  std::atomic<bool> running{false};
  HealthInfo health{};
  
  std::unique_ptr<RtspNv12Helper> rtsp_helper;
  
  // Frame buffer (pre-allocated scratch for FrameView)
  std::vector<uint8_t> rgb_frame_;  // 320×320×3
  
  Impl(const std::string& u, const RtspOptions& o) 
    : url(u), opts(o), rtsp_helper(new RtspNv12Helper) {}
};

RtspInputSource::RtspInputSource(std::string url, RtspOptions opts)
: p_(new Impl(std::move(url), opts)) {}

RtspInputSource::~RtspInputSource() { close(); }

bool RtspInputSource::open() noexcept {
  if (p_->opened.load(std::memory_order_acquire)) return true;
  
  // Wait for network connectivity before attempting RTSP connection
  LG_INFO("input_rtsp:checking network before opening RTSP stream\n");
  wait_for_network_with_validation(30, 2);
  
  if (!p_->rtsp_helper->open(p_->url, 3000)) {
    LG_ERROR("input_rtsp:open failed for %s\n", p_->url.c_str());
    return false;
  }

  p_->rgb_frame_.assign(320 * 320 * 3, 0);
  p_->opened.store(true, std::memory_order_release);
  LG_INFO("input_rtsp:open succeeded for %s\n", p_->url.c_str());
  return true;
}

bool RtspInputSource::start() noexcept {
  if (!p_->opened.load(std::memory_order_acquire)) return false;
  p_->running.store(true, std::memory_order_release);
  LG_INFO("input_rtsp:start called\n");
  return true;
}

void RtspInputSource::stop() noexcept {
  p_->running.store(false, std::memory_order_release);
}

void RtspInputSource::close() noexcept {
  p_->running.store(false, std::memory_order_release);
  if (p_->rtsp_helper) p_->rtsp_helper->close();
  p_->rgb_frame_.clear();
  p_->opened.store(false, std::memory_order_release);
  LG_INFO("input_rtsp:close called\n");
}

FetchStatus RtspInputSource::tryFetch(FrameView& out) noexcept {
  if (!p_->opened.load(std::memory_order_acquire) || !p_->running.load(std::memory_order_acquire)) {
    return FetchStatus::Broken;
  }

  if (p_->rtsp_helper->broken()) {
    return FetchStatus::Broken;
  }

  if (!p_->rtsp_helper->pull_into_rgb(p_->rgb_frame_.data())) {
    return FetchStatus::Timeout;
  }

  // Fill FrameView with BGR frame (matching USB camera format)
  out.fmt     = PixelFormat::BGR24;
  out.width   = 320;  // Preprocessed/resized dimensions
  out.height  = 320;
  out.stride0 = 320 * 3;
  out.stride1 = 0;
  out.plane0  = p_->rgb_frame_.data();
  out.plane1  = nullptr;
  out.pts_ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
  
  // V7.1: Store original RTSP stream dimensions for proper coordinate de-letterboxing
  // The frame has been resized to 320×320, but we need to preserve original dimensions
  // so that YOLOX can properly de-letterbox detections back to camera coordinates
  out.orig_width  = p_->rtsp_helper->width();   // Original 1920×1080 (or camera native res)
  out.orig_height = p_->rtsp_helper->height();
  
  // V7.1: Debug - verify original dimensions are being set correctly
  static int rtsp_fetch_debug = 0;
  if (rtsp_fetch_debug < 3) {
    LG_INFO("[RTSP-FETCH] out.width=%d out.height=%d out.orig_width=%d out.orig_height=%d",
            out.width, out.height, out.orig_width, out.orig_height);
    rtsp_fetch_debug++;
  }

  return FetchStatus::Ok;
}

FetchStatus RtspInputSource::fetch(FrameView& out, int timeout_ms) noexcept {
  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto st = tryFetch(out);
    if (st == FetchStatus::Ok || st == FetchStatus::Error) return st;
    if (timeout_ms <= 0) return st;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (elapsed >= timeout_ms) return FetchStatus::Timeout;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

HealthInfo RtspInputSource::getHealth() const noexcept { return p_->health; }

