#ifndef INPUT_FACTORY_H
#define INPUT_FACTORY_H

#include "input_source.h"
#include "input_rtsp.h"
#include "input_usb.h"
#include "input_file.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <utility>

struct InputConfig {
  // Exactly one of the following should be set for type selection:
  std::string rtsp_url;       // e.g., "rtsp://..."
  std::string usb_device;     // e.g., "/dev/video0" or "0"
  std::string file_path;      // e.g., "/storage/sd/video.mp4"

  UsbOptions  usb{};
  RtspOptions rtsp{};
  FileOptions file{};
};

// Helper: heuristics to classify a "hint" string
inline bool looks_like_rtsp(const std::string& s) {
  return s.rfind("rtsp://", 0) == 0 || s.rfind("rtsps://", 0) == 0;
}
inline bool looks_like_usb(const std::string& s) {
  if (s.rfind("/dev/video", 0) == 0) return true;
  if (s == "video0" || s == "video1") return true;
  return !s.empty() && std::all_of(s.begin(), s.end(),
                                   [](unsigned char c){ return std::isdigit(c); });
}
inline bool looks_like_file(const std::string& s) {
  return !s.empty() && !looks_like_rtsp(s) && !looks_like_usb(s);
}

// Create a source from a structured config (preferred)
inline std::unique_ptr<IInputSource> make_input(const InputConfig& cfg) {
  if (!cfg.rtsp_url.empty())
    return std::make_unique<RtspInputSource>(cfg.rtsp_url, cfg.rtsp);
  if (!cfg.usb_device.empty())
    return std::make_unique<UsbInputSource>(cfg.usb_device, cfg.usb);
  if (!cfg.file_path.empty())
    return std::make_unique<FileInputSource>(cfg.file_path, cfg.file);
  return nullptr;
}

// Convenience: infer type from a single string "hint"
inline std::unique_ptr<IInputSource> make_input_from_hint(const std::string& hint) {
  if (looks_like_rtsp(hint))
    return std::make_unique<RtspInputSource>(hint, RtspOptions{});
  if (looks_like_usb(hint))
    return std::make_unique<UsbInputSource>(hint, UsbOptions{});
  if (looks_like_file(hint))
    return std::make_unique<FileInputSource>(hint, FileOptions{});
  return nullptr;
}

#endif // INPUT_FACTORY_H
