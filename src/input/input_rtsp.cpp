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

// ===============================
// GStreamer One-time Init
// ===============================
static void gst_init_once() {
    static std::once_flag f;
    std::call_once(f, []{
        int argc = 0; char** argv = nullptr;
        gst_init(&argc, &argv);
        if (!getenv("GST_REGISTRY")) setenv("GST_REGISTRY", "/tmp/gst-registry.bin", 1);
    });
}

static void ensure_gstreamer_runtime() {
    const char* local = "/var/volatile/bsext/ext_npu_gaze/RK3588/lib/gstreamer-1.0";
    const char* sys   = "/usr/lib/gstreamer-1.0";
    std::string plugin_path = std::string(local) + ":" + sys;

    setenv("GST_PLUGIN_PATH", plugin_path.c_str(), 1);
    if (!getenv("GST_PLUGIN_SCANNER"))
        setenv("GST_PLUGIN_SCANNER", "/usr/libexec/gstreamer-1.0/gst-plugin-scanner", 1);
    if (!getenv("GST_REGISTRY"))
        setenv("GST_REGISTRY", "/tmp/gst-registry.bin", 1);
}

// ===============================
// Build RTSP GStreamer Pipelines
// ===============================
static std::vector<std::string> build_rtsp_pipelines(const std::string& url_in) {
    auto make_url_variants = [&](const std::string& u) {
        std::vector<std::string> urls;
        urls.push_back(u); // original
        // Try /stream2 first if available (Tapo friendly)
        try {
            auto pos = u.rfind("/stream1");
            if (pos != std::string::npos) {
                std::string u2 = u;
                u2.replace(pos, 8, "/stream2");
                urls.insert(urls.begin(), u2);
            }
        } catch (...) {}
        return urls;
    };

    auto mk = [&](const std::string& url, const char* proto, const char* enc, bool hw_decode, bool scaled_320, bool hw_scale) {
        std::string s =
            "rtspsrc location=" + url +
            " protocols=" + proto +
            " latency=150 drop-on-latency=true do-rtsp-keep-alive=true "
            " tcp-timeout=5000000000 timeout=7000000000 "
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
            s += "decodebin ! ";
        }

        if (scaled_320) {
            if (hw_scale) s += "rkvideoscale ! ";
            else          s += "videoscale ! ";
            s += "video/x-raw,format=NV12,width=320,height=320 ! ";
        }

        s += "video/x-raw,format=NV12 ! "
             "queue max-size-buffers=2 leaky=downstream ! "
             "appsink name=mysink caps=video/x-raw,format=NV12 "
             "drop=1 max-buffers=1 enable-last-sample=false sync=false";
        return s;
    };

    std::vector<std::string> p;
    auto urls = make_url_variants(url_in);

    for (const auto& url : urls) {
        // H264, HW decode, no scaling (fast)
        p.push_back(mk(url, "udp", "H264", /*hw_decode=*/true,  /*scaled_320=*/false, /*hw_scale=*/false));
        p.push_back(mk(url, "tcp", "H264", /*hw_decode=*/true,  /*scaled_320=*/false, /*hw_scale=*/false));
        // With upstream scaling
        p.push_back(mk(url, "udp", "H264", /*hw_decode=*/true,  /*scaled_320=*/true,  /*hw_scale=*/true));
        p.push_back(mk(url, "tcp", "H264", /*hw_decode=*/true,  /*scaled_320=*/true,  /*hw_scale=*/true));
        p.push_back(mk(url, "udp", nullptr,/*hw_decode=*/true,  /*scaled_320=*/true,  /*hw_scale=*/true));
        p.push_back(mk(url, "tcp", nullptr,/*hw_decode=*/true,  /*scaled_320=*/true,  /*hw_scale=*/true));
        // Software decode fallback
        p.push_back(mk(url, "tcp", "H264", /*hw_decode=*/false, /*scaled_320=*/false, /*hw_scale=*/false));
        p.push_back(mk(url, "tcp", "H264", /*hw_decode=*/false, /*scaled_320=*/true,  /*hw_scale=*/false));
        // H265
        p.push_back(mk(url, "tcp", "H265", /*hw_decode=*/true,  /*scaled_320=*/false, /*hw_scale=*/false));
        p.push_back(mk(url, "tcp", "H265", /*hw_decode=*/true,  /*scaled_320=*/true,  /*hw_scale=*/true));
    }
    return p;
}

// ===============================
// RtspNv12 Helper (GStreamer Wrapper)
// ===============================
class RtspNv12Helper {
public:
    ~RtspNv12Helper() { close(); }

    bool open(const std::string& url, int first_frame_timeout_ms = 3000) {
        close();
        gst_init_once();
        ensure_gstreamer_runtime();

        auto pipelines = build_rtsp_pipelines(url);
        GError* err = nullptr;

        for (const auto& pipeline_str : pipelines) {
            LG_INFO("input_rtsp:trying pipeline: %s\n", pipeline_str.substr(0, 120).c_str());

            err = nullptr;
            pipeline_ = gst_parse_launch(pipeline_str.c_str(), &err);
            if (!pipeline_) {
                if (err) { g_error_free(err); err = nullptr; }
                continue;
            }

            appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mysink");
            if (!appsink_) {
                gst_object_unref(pipeline_);
                pipeline_ = nullptr;
                continue;
            }

            GstCaps* caps = gst_caps_from_string("video/x-raw,format=NV12");
            g_object_set(G_OBJECT(appsink_),
                        "caps", caps,
                        "drop", 1,
                        "max-buffers", 1,
                        "enable-last-sample", FALSE,
                        "sync", FALSE,
                        nullptr);
            gst_caps_unref(caps);

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
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), first_frame_timeout_ms * GST_MSECOND);
            if (!sample) {
                LG_WARN("input_rtsp:first-frame timeout (%d ms) on this pipeline\n", first_frame_timeout_ms);
                close();
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
                // NV12 (W×H) -> NV12 (320×320) via RGA
                rga_buffer_t src_nv12       = wrapbuffer_virtualaddr((void*)src_base, W, H, RK_FORMAT_YCbCr_420_SP);
                rga_buffer_t dst_nv12_small = wrapbuffer_virtualaddr(nv12_small_.data(), 320, 320, RK_FORMAT_YCbCr_420_SP);
                double fx = 320.0 / (double)W;
                double fy = 320.0 / (double)H;
                int ret = imresize(src_nv12, dst_nv12_small, fx, fy, 0, IM_SYNC);
                if (ret != IM_STATUS_SUCCESS) break;

                // NV12 (320×320) -> BGR (320×320) via RGA (to match USB camera BGR format)
                rga_buffer_t src_small = wrapbuffer_virtualaddr(nv12_small_.data(), 320, 320, RK_FORMAT_YCbCr_420_SP);
                rga_buffer_t dst_bgr   = wrapbuffer_virtualaddr(rgb_out_320x320, 320, 320, RK_FORMAT_BGR_888);
                ret = imcvtcolor(src_small, dst_bgr, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888, IM_SYNC);
                if (ret != IM_STATUS_SUCCESS) break;

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
  out.width   = 320;
  out.height  = 320;
  out.stride0 = 320 * 3;
  out.stride1 = 0;
  out.plane0  = p_->rgb_frame_.data();
  out.plane1  = nullptr;
  out.pts_ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

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

