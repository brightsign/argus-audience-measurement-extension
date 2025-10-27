#include "input/input_from_registry.h"
#include "input/input_factory.h"
#include <cctype>
#include <string>
#include <dirent.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

// --- helpers ---
static std::string to_lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
static bool starts_with(const std::string& s, const char* pfx) { return s.rfind(pfx, 0) == 0; }
static bool looks_like_path(const std::string& s) {
  return !s.empty() && (s[0]=='/' || starts_with(s,"./") || starts_with(s,"../"));
}

static bool looks_like_usb_token(const std::string& s) {
  return (s=="usb_camera" || s=="usb" || s=="camera");
}
static bool looks_like_camera_index(const std::string& s) {
  // "0", "1", etc.
  if (s.empty()) return false;
  for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

static bool xioctl(int fd, unsigned long req, void* arg) {
  for (int i=0;i<4;++i) {
    if (ioctl(fd, req, arg) != -1) return true;
    if (errno==EINTR || errno==EAGAIN) continue;
    break;
  }
  return false;
}

static std::string autoDetectUsbDeviceV4L2() {
  // collect /dev/video*
  std::vector<std::string> devs;
  if (DIR* d = ::opendir("/dev")) {
    while (dirent* e = ::readdir(d)) {
      if (!e->d_name) continue;
      std::string name = e->d_name;
      if (name.rfind("video",0)==0) devs.emplace_back("/dev/"+name);
    }
    ::closedir(d);
  }
  std::sort(devs.begin(), devs.end());

  for (const auto& dev : devs) {
    int fd = ::open(dev.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) continue;

    v4l2_capability cap{};
    if (!xioctl(fd, VIDIOC_QUERYCAP, &cap)) { ::close(fd); continue; }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) { ::close(fd); continue; }

    // optional: attempt to set NV12 (skip if your pipeline handles other formats)
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    // If this fails, still accept the device (format may be set later in open())
    (void)xioctl(fd, VIDIOC_S_FMT, &fmt);

    ::close(fd);
    return dev; // first working device
  }
  return std::string{};
}

// src/input/input_from_registry.cpp (RTSP section simplified)
InputConfig make_input_from_registry_value(const std::string& raw) {
  InputConfig ic{};  // zero-init

  const auto to_lower = [](std::string s){
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const auto starts_with = [](const std::string& s, const char* pfx){ return s.rfind(pfx,0)==0; };
  const auto looks_like_rtsp = [&](const std::string& s){
    const auto low = to_lower(s);
    return starts_with(low, "rtsp://") || starts_with(low, "rtsps://");
  };
  const auto looks_like_path = [&](const std::string& s){
    return !s.empty() && (s[0]=='/' || starts_with(s,"./") || starts_with(s,"../"));
  };

  const std::string low = to_lower(raw);

  // USB tokens / device nodes / numeric index
  if (low=="usb_camera" || low=="usb" || low=="camera") {
    std::string dev = autoDetectUsbDeviceV4L2();
    ic.usb_device = !dev.empty() ? dev : "/dev/video0";
    ic.usb.width  = ic.usb.width  > 0 ? ic.usb.width  : 640;
    ic.usb.height = ic.usb.height > 0 ? ic.usb.height : 480;
    ic.usb.fps    = ic.usb.fps    > 0 ? ic.usb.fps    : 30;
    return ic;
  }

  // RTSP URL — only set what you actually have
  if (looks_like_rtsp(raw)) {
    ic.rtsp_url = raw;
    // If your RtspOptions has latency_ms or transport options, set them here.
    // Otherwise, leave ic.rtsp as defaults.
    return ic;
  }

  // File path
  if (looks_like_path(raw)) {
    ic.file_path = raw;
    // If your FileOptions has loop, set it here (ic.file.loop = true;)
    return ic;
  }

  // Fallback to USB
  ic.usb_device = "/dev/video0";
  ic.usb.width = 640; ic.usb.height = 480; ic.usb.fps = 30;
  return ic;
}
