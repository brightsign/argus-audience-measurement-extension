#include "input/input_file.h"
#include "input/media_probe.h"
#include "metrics/log_global.h"
#include <opencv2/opencv.hpp>
#include <gst/gst.h>
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>
#include <cstring>

struct FileInputSource::Impl {
  std::string path;
  FileOptions opts;
  cv::VideoCapture cap;
  std::atomic<bool> opened{false};
  std::atomic<bool> running{false};
  std::atomic<bool> broken{false};
  std::atomic<uint64_t> frames_ok{0};
  std::atomic<uint64_t> last_ok_ns{0};
  std::vector<uint8_t> scratch_bgr;
  int width{0};
  int height{0};
  uint64_t frame_count{0};
  bool input_is_nv12{false};  // Flag for NV12 format from GStreamer
};

FileInputSource::FileInputSource(std::string path, FileOptions opts)
: p_(new Impl{std::move(path), opts}) {
  LG_INFO("FileInputSource: created for path=%s loop=%d", p_->path.c_str(), opts.loop);
}

FileInputSource::~FileInputSource() {
  if (p_->cap.isOpened()) {
    p_->cap.release();
  }
}

bool FileInputSource::open() noexcept {
  if (p_->path.empty()) {
    LG_ERROR("FileInputSource: empty file path");
    return false;
  }
  
  LG_INFO("FileInputSource: opening file: %s", p_->path.c_str());
  
  // VideoCaptureAPIs numeric constants (for compatibility with older OpenCV)
  const int CAP_ANY = 0;
  const int CAP_FFMPEG = 1900;
  const int CAP_GSTREAMER = 1800;
  
  // Helper lambdas
  auto try_cv_backend = [&](int api, const char* name) -> bool {
    p_->cap.release();
    p_->cap.open(p_->path, api);
    if (p_->cap.isOpened()) {
      LG_INFO("FileInputSource: opened with %s backend", name);
      return true;
    }
    return false;
  };
  
  auto try_gst_pipeline = [&](const std::string& pipe, const char* tag) -> bool {
    p_->cap.release();
    p_->cap.open(pipe, CAP_GSTREAMER);
    if (p_->cap.isOpened()) {
      LG_INFO("FileInputSource: opened via %s pipeline", tag);
      return true;
    } else {
      LG_WARN("FileInputSource: pipeline failed (%s)", tag);
      return false;
    }
  };
  
  // --- 0) Quick tries with OpenCV codecs (if ffmpeg is built in)
  if (try_cv_backend(CAP_FFMPEG, "CAP_FFMPEG")) {
    goto opened_ok;
  }
  LG_WARN("FileInputSource: CAP_FFMPEG failed, trying default backend");
  if (try_cv_backend(CAP_ANY, "CAP_ANY")) {
    goto opened_ok;
  }
  
  // --- GStreamer pipeline attempts (in a scope to allow goto)
  {
    auto has_elem = [](const char* name) -> bool {
      // Check if GStreamer element exists
      GstElementFactory* f = gst_element_factory_find(name);
      if (!f) return false;
      gst_object_unref(f);
      return true;
    };
    
    // --- 1) Probe the media to detect codec and resolution
    std::string preferred_codec;  // "h264" | "h265" | "vp9" | ""
    int detected_width = 0;
    int detected_height = 0;
    
    if (auto pr = probe_media_via_gst(p_->path)) {
      LG_INFO("MediaProbe: container=%s video=%s %dx%d @ %.2f fps",
              pr->container_caps.c_str(), pr->video_caps.c_str(),
              pr->width, pr->height, pr->fps);
      
      detected_width = pr->width;
      detected_height = pr->height;
      
      const std::string& vc = pr->video_caps;
      // Detect codec from caps string
      if (vc.find("video/x-h264") != std::string::npos || vc.find("avc") != std::string::npos) {
        preferred_codec = "h264";
      } else if (vc.find("video/x-h265") != std::string::npos || vc.find("hevc") != std::string::npos) {
        preferred_codec = "h265";
      } else if (vc.find("video/x-vp9") != std::string::npos || vc.find("vp9") != std::string::npos) {
        preferred_codec = "vp9";
      } else if (vc.find("video/x-vp8") != std::string::npos || vc.find("vp8") != std::string::npos) {
        preferred_codec = "vp8";
      }
    } else {
      LG_WARN("MediaProbe: unable to detect codec; will try heuristic order");
    }
    
    // --- 2) GStreamer availability snapshot
    bool filesrc_avail         = has_elem("filesrc");
    bool qtdemux_avail         = has_elem("qtdemux");
    bool matroskademux_avail   = has_elem("matroskademux");  // For WebM/MKV
    bool tsdemux_avail         = has_elem("tsdemux");         // For MPEG-TS
    bool h264parse_avail       = has_elem("h264parse");
    bool h265parse_avail       = has_elem("h265parse");
    bool vp9parse_avail        = has_elem("vp9parse");
    bool vp8parse_avail        = has_elem("vp8parse");
    bool avdec_h264_avail      = has_elem("avdec_h264");
    bool avdec_h265_avail      = has_elem("avdec_h265");
    bool avdec_vp9_avail       = has_elem("avdec_vp9");
    bool avdec_vp8_avail       = has_elem("avdec_vp8");
    bool mppvideodec_avail     = has_elem("mppvideodec");
    bool videoconvert_avail    = has_elem("videoconvert");
    bool appsink_avail         = has_elem("appsink");
    bool decodebin_avail       = has_elem("decodebin");
    
    LG_INFO("GStreamer elements: qtdemux=%d matroskademux=%d tsdemux=%d h264parse=%d h265parse=%d vp9parse=%d "
            "mppvideodec=%d avdec_h264=%d avdec_h265=%d avdec_vp9=%d videoconvert=%d appsink=%d decodebin=%d",
            qtdemux_avail, matroskademux_avail, tsdemux_avail, h264parse_avail, h265parse_avail, vp9parse_avail,
            mppvideodec_avail, avdec_h264_avail, avdec_h265_avail, avdec_vp9_avail, videoconvert_avail, appsink_avail, decodebin_avail);
    
    // Helper to build BGR output tail
    auto bgr_tail = []() -> std::string {
      return " ! videoconvert ! video/x-raw,format=BGR ! "
             "appsink drop=1 max-buffers=1 sync=false";
    };
    
    // Helper for VP9 with automatic downscaling for performance
    auto bgr_tail_vp9_optimized = [](int width, int height) -> std::string {
      // For VP9 software decode, downscale to 720p or less for better performance
      if (width > 1280 || height > 720) {
        int target_w = (width > 1920) ? 1280 : (width / 2);
        int target_h = (height > 1080) ? 720 : (height / 2);
        return " ! videoscale ! video/x-raw,width=" + std::to_string(target_w) + 
               ",height=" + std::to_string(target_h) + 
               " ! videoconvert ! video/x-raw,format=BGR ! "
               "appsink drop=1 max-buffers=1 sync=false";
      }
      return " ! videoconvert ! video/x-raw,format=BGR ! "
             "appsink drop=1 max-buffers=1 sync=false";
    };
    
    // --- 3) Build codec-ordered pipeline attempts
    std::vector<std::string> codec_order;
    if (preferred_codec == "h264") {
      codec_order = {"h264", "h265", "vp9", "vp8"};
    } else if (preferred_codec == "h265") {
      codec_order = {"h265", "h264", "vp9", "vp8"};
    } else if (preferred_codec == "vp9") {
      codec_order = {"vp9", "h264", "h265", "vp8"};
    } else if (preferred_codec == "vp8") {
      codec_order = {"vp8", "vp9", "h264", "h265"};
    } else {
      codec_order = {"h264", "h265", "vp9", "vp8"};  // default heuristic
    }
    
    const std::string file = p_->path;
    
    // Detect container format from file extension
    std::string demuxer_hint;
    if (file.find(".webm") != std::string::npos || file.find(".mkv") != std::string::npos) {
      demuxer_hint = "matroska";
    } else if (file.find(".ts") != std::string::npos || file.find(".mts") != std::string::npos) {
      demuxer_hint = "mpegts";
    } else if (file.find(".mp4") != std::string::npos || file.find(".mov") != std::string::npos) {
      demuxer_hint = "mp4";
    }
    
    // --- Try alternative demuxers first (WebM/MKV or MPEG-TS)
    if (demuxer_hint == "matroska" && matroskademux_avail && filesrc_avail && videoconvert_avail && appsink_avail) {
      LG_INFO("FileInputSource: detected WebM/MKV, trying matroskademux");
      
      // Try codec-specific pipelines for WebM/MKV
      if (preferred_codec == "vp9" && vp9parse_avail && avdec_vp9_avail) {
        std::string pipe = "filesrc location=\"" + file + "\" ! matroskademux ! "
                          "vp9parse ! avdec_vp9" + bgr_tail();
        if (try_gst_pipeline(pipe, "WebM VP9")) goto opened_ok;
      }
      if (preferred_codec == "vp8" && vp8parse_avail && avdec_vp8_avail) {
        std::string pipe = "filesrc location=\"" + file + "\" ! matroskademux ! "
                          "vp8parse ! avdec_vp8" + bgr_tail();
        if (try_gst_pipeline(pipe, "WebM VP8")) goto opened_ok;
      }
      if (preferred_codec == "h264" && h264parse_avail) {
        if (mppvideodec_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! matroskademux ! "
                            "h264parse ! mppvideodec" + bgr_tail();
          if (try_gst_pipeline(pipe, "MKV H.264 HW")) goto opened_ok;
        }
        if (avdec_h264_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! matroskademux ! "
                            "h264parse ! avdec_h264" + bgr_tail();
          if (try_gst_pipeline(pipe, "MKV H.264 SW")) goto opened_ok;
        }
      }
    }
    
    if (demuxer_hint == "mpegts" && tsdemux_avail && filesrc_avail && videoconvert_avail && appsink_avail) {
      LG_INFO("FileInputSource: detected MPEG-TS, trying tsdemux");

      // Try codec-specific pipelines for MPEG-TS
      // If codec detected, try that first; otherwise try all supported codecs
      bool try_h264 = (preferred_codec == "h264" || preferred_codec.empty());
      bool try_h265 = (preferred_codec == "h265" || preferred_codec.empty());

      if (try_h264 && h264parse_avail) {
        if (mppvideodec_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! tsdemux ! "
                            "h264parse ! mppvideodec" + bgr_tail();
          if (try_gst_pipeline(pipe, "MPEG-TS H.264 HW")) goto opened_ok;
        }
        if (avdec_h264_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! tsdemux ! "
                            "h264parse ! avdec_h264" + bgr_tail();
          if (try_gst_pipeline(pipe, "MPEG-TS H.264 SW")) goto opened_ok;
        }
      }
      if (try_h265 && h265parse_avail) {
        if (mppvideodec_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! tsdemux ! "
                            "h265parse ! mppvideodec" + bgr_tail();
          if (try_gst_pipeline(pipe, "MPEG-TS H.265 HW")) goto opened_ok;
        }
        if (avdec_h265_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! tsdemux ! "
                            "h265parse ! avdec_h265" + bgr_tail();
          if (try_gst_pipeline(pipe, "MPEG-TS H.265 SW")) goto opened_ok;
        }
      }
    }
    
    // --- Special case: if qtdemux is missing but decodebin is available, use it first
    if (!qtdemux_avail && decodebin_avail && videoconvert_avail && appsink_avail) {
      LG_WARN("FileInputSource: qtdemux missing, trying decodebin for auto-demux/decode");
      std::string pipe = "filesrc location=\"" + file + "\" ! decodebin ! "
                        "videoconvert ! video/x-raw,format=BGR ! "
                        "appsink drop=1 max-buffers=1 sync=false";
      if (try_gst_pipeline(pipe, "Auto (decodebin - no qtdemux)")) goto opened_ok;
    }
    
    for (const auto& codec : codec_order) {
      if (codec == "h264" && filesrc_avail && qtdemux_avail && h264parse_avail) {
        // Try HW decode first (Rockchip MPP)
        if (mppvideodec_avail && videoconvert_avail && appsink_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! qtdemux ! "
                            "h264parse config-interval=1 disable-passthrough=true ! "
                            "mppvideodec" + bgr_tail();
          if (try_gst_pipeline(pipe, "H.264 HW (mppvideodec)")) goto opened_ok;
        }
        // Try SW decode fallback
        if (avdec_h264_avail && videoconvert_avail && appsink_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! qtdemux ! "
                            "h264parse config-interval=1 disable-passthrough=true ! "
                            "avdec_h264" + bgr_tail();
          if (try_gst_pipeline(pipe, "H.264 SW (avdec_h264)")) goto opened_ok;
        }
      }
      else if (codec == "h265" && filesrc_avail && qtdemux_avail && h265parse_avail) {
        // Try HW decode first
        if (mppvideodec_avail && videoconvert_avail && appsink_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! qtdemux ! "
                            "h265parse disable-passthrough=true ! "
                            "mppvideodec" + bgr_tail();
          if (try_gst_pipeline(pipe, "H.265 HW (mppvideodec)")) goto opened_ok;
        }
        // Try SW decode
        if (avdec_h265_avail && videoconvert_avail && appsink_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! qtdemux ! "
                            "h265parse disable-passthrough=true ! "
                            "avdec_h265" + bgr_tail();
          if (try_gst_pipeline(pipe, "H.265 SW (avdec_h265)")) goto opened_ok;
        }
      }
      else if (codec == "vp9" && filesrc_avail && qtdemux_avail && avdec_vp9_avail && videoconvert_avail && appsink_avail) {
        // VP9: software decode only - use optimized downscaling for 1080p+
        if (detected_width > 1280 || detected_height > 720) {
          LG_WARN("FileInputSource: VP9 %dx%d detected - auto-downscaling for performance", detected_width, detected_height);
          LG_INFO("  (For best performance, convert to H.264: ffmpeg -i input.mp4 -c:v libx264 -crf 23 output.mp4)");
        }
        
        if (vp9parse_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! qtdemux ! "
                            "vp9parse ! avdec_vp9" + bgr_tail_vp9_optimized(detected_width, detected_height);
          if (try_gst_pipeline(pipe, "VP9 SW (optimized)")) goto opened_ok;
        }
        {
          std::string pipe = "filesrc location=\"" + file + "\" ! qtdemux ! "
                            "avdec_vp9" + bgr_tail_vp9_optimized(detected_width, detected_height);
          if (try_gst_pipeline(pipe, "VP9 SW (optimized)")) goto opened_ok;
        }
        // Try HW MPP if available (uncommon for VP9 on RK3568)
        if (mppvideodec_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! qtdemux ! "
                            "vp9parse ! mppvideodec" + bgr_tail();
          if (try_gst_pipeline(pipe, "VP9 HW (mppvideodec)")) goto opened_ok;
        }
      }
      else if (codec == "vp8" && filesrc_avail && qtdemux_avail && avdec_vp8_avail && videoconvert_avail && appsink_avail) {
        // VP8: similar to VP9
        if (vp8parse_avail) {
          std::string pipe = "filesrc location=\"" + file + "\" ! qtdemux ! "
                            "vp8parse ! avdec_vp8" + bgr_tail();
          if (try_gst_pipeline(pipe, "VP8 SW (avdec_vp8 + vp8parse)")) goto opened_ok;
        }
        {
          std::string pipe = "filesrc location=\"" + file + "\" ! qtdemux ! "
                            "avdec_vp8" + bgr_tail();
          if (try_gst_pipeline(pipe, "VP8 SW (avdec_vp8)")) goto opened_ok;
        }
      }
    }
    
    // --- 4) Last-ditch: generic decodebin
    if (filesrc_avail && decodebin_avail && videoconvert_avail && appsink_avail) {
      std::string pipe = "filesrc location=\"" + file + "\" ! decodebin ! "
                        "videoconvert ! video/x-raw,format=BGR ! "
                        "appsink drop=1 max-buffers=1 sync=false";
      if (try_gst_pipeline(pipe, "Generic (decodebin)")) goto opened_ok;
    }
    
    // --- 5) All GStreamer attempts failed - give detailed error
    LG_ERROR("FileInputSource: failed to open file with any backend: %s", p_->path.c_str());
    
    // Provide specific guidance based on what's missing
    if (!qtdemux_avail && !decodebin_avail) {
      LG_ERROR("FileInputSource: CRITICAL - both qtdemux and decodebin missing!");
      LG_ERROR("  For MP4/MOV files, you need these GStreamer plugins:");
      LG_ERROR("    - libgstisomp4.so  (provides qtdemux for MP4 demuxing)");
      LG_ERROR("    - libgstplayback.so (provides decodebin/uridecodebin)");
      LG_ERROR("  ");
      LG_ERROR("  ALTERNATIVE: Convert file to supported format:");
      if (matroskademux_avail) {
        LG_ERROR("    - WebM/MKV: ffmpeg -i input.mp4 -c:v copy output.webm");
      }
      if (tsdemux_avail) {
        LG_ERROR("    - MPEG-TS: ffmpeg -i input.mp4 -c:v libx264 -an output.ts");
      }
      if (!matroskademux_avail && !tsdemux_avail) {
        LG_ERROR("    - Install gst-plugins-good (matroskademux) or gst-plugins-bad (tsdemux)");
      }
    } else if (!qtdemux_avail) {
      LG_ERROR("FileInputSource: qtdemux missing (MP4/MOV demuxer)");
      LG_ERROR("  Install libgstisomp4.so or convert file:");
      if (matroskademux_avail) {
        LG_ERROR("    - WebM: ffmpeg -i input.mp4 -c:v copy output.webm");
      }
      if (tsdemux_avail) {
        LG_ERROR("    - MPEG-TS: ffmpeg -i input.mp4 -c:v libx264 -an output.ts");
      }
    } else if (!decodebin_avail) {
      LG_ERROR("FileInputSource: decodebin missing (auto-decoder)");
      LG_ERROR("  Install libgstplayback.so to /var/volatile/bsext/ext_npu_argus/RK3568/lib/gstreamer-1.0/");
    }
    
    LG_ERROR("FileInputSource: check file codec (H.264/H.265/VP9/VP8) and these plugins:");
    LG_ERROR("  Required: videoconvert, appsink");
    LG_ERROR("  Container: qtdemux (MP4), matroskademux (WebM/MKV), or tsdemux (MPEG-TS)");
    LG_ERROR("  H.264: h264parse, avdec_h264 (or mppvideodec)");
    LG_ERROR("  H.265: h265parse, avdec_h265 (or mppvideodec)");
    LG_ERROR("  VP9: avdec_vp9, vp9parse (optional)");
    LG_ERROR("  VP8: avdec_vp8, vp8parse (optional)");
    LG_ERROR("  Verify with: gst-inspect-1.0 matroskademux tsdemux");
  } // end GStreamer scope
  
  p_->broken.store(true);
  return false;

opened_ok:
  // VideoCapture property constants (for compatibility)
  const int CAP_PROP_FRAME_WIDTH = 3;
  const int CAP_PROP_FRAME_HEIGHT = 4;
  const int CAP_PROP_FPS = 5;
  
  p_->width = (int)p_->cap.get(CAP_PROP_FRAME_WIDTH);
  p_->height = (int)p_->cap.get(CAP_PROP_FRAME_HEIGHT);
  double fps = p_->cap.get(CAP_PROP_FPS);
  
  if (p_->width <= 0 || p_->height <= 0) {
    LG_ERROR("FileInputSource: invalid dimensions %dx%d", p_->width, p_->height);
    p_->cap.release();
    p_->broken.store(true);
    return false;
  }
  
  // Don't treat fps=0 as fatal on embedded systems
  if (fps <= 0) {
    LG_WARN("FileInputSource: FPS reported as %.1f (may be inaccurate on embedded)", fps);
    fps = 30.0; // Assume reasonable default
  }
  
  LG_INFO("FileInputSource: opened %s - %dx%d @ %.1f fps", 
          p_->path.c_str(), p_->width, p_->height, fps);
  
  p_->opened.store(true);
  p_->broken.store(false);
  p_->input_is_nv12 = false;  // Always using BGR output
  return true;
}

bool FileInputSource::start() noexcept {
  if (!p_->opened.load()) {
    LG_WARN("FileInputSource: not opened, cannot start");
    return false;
  }
  p_->running.store(true);
  LG_INFO("FileInputSource: started");
  return true;
}

void FileInputSource::stop() noexcept {
  p_->running.store(false);
  LG_INFO("FileInputSource: stopped");
}

void FileInputSource::close() noexcept {
  if (p_->cap.isOpened()) {
    p_->cap.release();
  }
  p_->opened.store(false);
  LG_INFO("FileInputSource: closed");
}

FetchStatus FileInputSource::tryFetch(FrameView& out) noexcept {
  if (!p_->running.load()) {
    return FetchStatus::Timeout;
  }
  
  if (!p_->cap.isOpened()) {
    return FetchStatus::Error;
  }
  
  cv::Mat frame;
  bool success = p_->cap.read(frame);
  
  if (!success || frame.empty()) {
    // End of file reached
    if (p_->opts.loop) {
      // Loop back to beginning
      LG_INFO("FileInputSource: end of file, looping back (frame_count=%llu)", 
              (unsigned long long)p_->frame_count);
      const int CAP_PROP_POS_FRAMES = 1;  // Position in frames
      p_->cap.set(CAP_PROP_POS_FRAMES, 0);
      p_->frame_count = 0;
      
      // Try reading first frame again
      success = p_->cap.read(frame);
      if (!success || frame.empty()) {
        LG_ERROR("FileInputSource: failed to read first frame after loop");
        p_->broken.store(true);
        return FetchStatus::Error;
      }
    } else {
      LG_INFO("FileInputSource: end of file reached (no loop)");
      return FetchStatus::Timeout;
    }
  }
  
  // Convert to BGR if needed
  cv::Mat bgr;
  if (p_->input_is_nv12) {
    // NV12 format from GStreamer appsink (when videoconvert is not available)
    // NV12 is stored as height*1.5 rows, single channel
    if (frame.rows == p_->height * 3 / 2 && frame.channels() == 1) {
      cv::cvtColor(frame, bgr, cv::COLOR_YUV2BGR_NV12);
    } else {
      // Might already be BGR despite the flag
      if (frame.channels() == 3) {
        bgr = frame;
      } else {
        LG_ERROR("FileInputSource: unexpected frame format for NV12 conversion");
        return FetchStatus::Error;
      }
    }
  } else if (frame.channels() == 3) {
    bgr = frame;
  } else if (frame.channels() == 4) {
    cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
  } else {
    cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
  }
  
  // Ensure continuous memory
  if (!bgr.isContinuous()) {
    bgr = bgr.clone();
  }
  
  // Copy to scratch buffer
  size_t size_bytes = bgr.total() * bgr.elemSize();
  p_->scratch_bgr.resize(size_bytes);
  std::memcpy(p_->scratch_bgr.data(), bgr.data, size_bytes);
  
  // Fill FrameView
  auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  
  out.fmt     = PixelFormat::BGR24;
  out.width   = bgr.cols;
  out.height  = bgr.rows;
  out.orig_width  = bgr.cols;   // V7.1: Store original dimensions for visualization
  out.orig_height = bgr.rows;
  out.stride0 = bgr.cols * 3;
  out.stride1 = 0;
  out.plane0  = p_->scratch_bgr.data();
  out.plane1  = nullptr;
  out.pts_ns  = now_ns;
  
  p_->frames_ok.fetch_add(1, std::memory_order_relaxed);
  p_->last_ok_ns.store(now_ns, std::memory_order_relaxed);
  p_->frame_count++;
  
  return FetchStatus::Ok;
}

FetchStatus FileInputSource::fetch(FrameView& out, int timeout_ms) noexcept {
  auto start = std::chrono::steady_clock::now();
  
  while (true) {
    auto status = tryFetch(out);
    if (status == FetchStatus::Ok || status == FetchStatus::Error) {
      return status;
    }
    
    if (timeout_ms <= 0) {
      return status;
    }
    
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    
    if (elapsed_ms >= timeout_ms) {
      return FetchStatus::Timeout;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

HealthInfo FileInputSource::getHealth() const noexcept {
  HealthInfo h{};
  h.frames_ok = p_->frames_ok.load(std::memory_order_relaxed);
  h.last_ok_ns = p_->last_ok_ns.load(std::memory_order_relaxed);
  h.connected = p_->cap.isOpened();
  
  if (p_->broken.load()) {
    h.status = HealthStatus::Error;
  } else if (!h.connected) {
    h.status = HealthStatus::Disconnected;
  } else {
    h.status = HealthStatus::Ok;
  }
  
  return h;
}

