#pragma once
#include <string>
#include <optional>

struct MediaProbeResult {
  std::string container_caps;  // e.g. "video/quicktime"
  std::string video_caps;      // e.g. "video/x-h264", "video/x-h265", "video/x-vp9"
  int width{0};
  int height{0};
  double fps{0.0};
};

std::optional<MediaProbeResult> probe_media_via_gst(const std::string& path);
