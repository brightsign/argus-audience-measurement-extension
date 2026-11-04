#pragma once
// Small, OpenCV-free helper for optional V4L2 camera controls (USB sources only).
namespace v4l2ctl {

enum class ApplyResult {
  Applied,       // control set and read back equal to the request
  Mismatch,      // set succeeded but the camera reports a different value
  Unsupported,   // control missing or disabled on this device (never fatal)
  Failed         // control exists but VIDIOC_S_CTRL failed (never fatal)
};

// Set V4L2_CID_EXPOSURE_AUTO_PRIORITY (0 = keep frame rate constant in low light, 1 = camera may lower fps for
// exposure) on an open V4L2 fd. `want` must be 0 or 1. `before` / `actual` are the values read before the change
// and after it (-1 when unreadable).
ApplyResult set_exposure_auto_priority(int fd, int want, int* before, int* actual) noexcept;

}  // namespace v4l2ctl
