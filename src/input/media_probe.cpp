#include "input/media_probe.h"
#include "metrics/log_global.h"
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

static std::string caps_to_string(const GstCaps* caps) {
  if (!caps) return {};
  gchar* s = gst_caps_to_string(caps);
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

std::optional<MediaProbeResult> probe_media_via_gst(const std::string& path) {
  // Ensure GStreamer is initialized
  static bool gst_initialized = false;
  if (!gst_initialized) {
    gst_init(nullptr, nullptr);
    gst_initialized = true;
  }

  GError* err = nullptr;
  GstDiscoverer* disc = gst_discoverer_new(5 * GST_SECOND, &err);
  if (!disc) {
    if (err) {
      LG_WARN("MediaProbe: failed to create discoverer: %s", err->message);
      g_error_free(err);
    }
    return std::nullopt;
  }

  // Convert path -> file:// URI
  gchar* uri = gst_filename_to_uri(path.c_str(), &err);
  if (!uri) {
    if (err) {
      LG_WARN("MediaProbe: failed to convert path to URI: %s", err->message);
      g_error_free(err);
    }
    gst_object_unref(disc);
    return std::nullopt;
  }

  GstDiscovererInfo* info = gst_discoverer_discover_uri(disc, uri, &err);
  g_free(uri);
  
  if (!info) {
    if (err) {
      LG_WARN("MediaProbe: failed to discover URI: %s", err->message);
      g_error_free(err);
    }
    gst_object_unref(disc);
    return std::nullopt;
  }

  GstDiscovererResult res = gst_discoverer_info_get_result(info);
  if (res != GST_DISCOVERER_OK && res != GST_DISCOVERER_MISSING_PLUGINS) {
    LG_WARN("MediaProbe: discovery failed with result %d", res);
    gst_discoverer_info_unref(info);
    gst_object_unref(disc);
    return std::nullopt;
  }

  MediaProbeResult out;

  // Get container caps (from top-level stream info)
  GstDiscovererStreamInfo* sinfo = gst_discoverer_info_get_stream_info(info);
  if (sinfo) {
    if (const GstCaps* c_caps = gst_discoverer_stream_info_get_caps(sinfo)) {
      out.container_caps = caps_to_string(c_caps);
    }
  }

  // Find the first video stream
  GList* streams = gst_discoverer_info_get_stream_list(info);
  for (GList* l = streams; l; l = l->next) {
    auto* si = static_cast<GstDiscovererStreamInfo*>(l->data);
    if (!si) continue;
    
    if (GST_IS_DISCOVERER_VIDEO_INFO(si)) {
      auto* v = GST_DISCOVERER_VIDEO_INFO(si);
      out.width  = gst_discoverer_video_info_get_width(v);
      out.height = gst_discoverer_video_info_get_height(v);

      // fps as double
      guint num = gst_discoverer_video_info_get_framerate_num(v);
      guint den = gst_discoverer_video_info_get_framerate_denom(v);
      if (num > 0 && den > 0) {
        out.fps = static_cast<double>(num) / den;
      }

      // Video caps string
      if (const GstCaps* vcaps = gst_discoverer_stream_info_get_caps(si)) {
        out.video_caps = caps_to_string(vcaps);
      }
      break;
    }
  }
  
  if (streams) {
    gst_discoverer_stream_info_list_free(streams);
  }
  
  if (sinfo) {
    gst_discoverer_stream_info_unref(sinfo);
  }
  gst_discoverer_info_unref(info);
  gst_object_unref(disc);
  
  return out;
}
