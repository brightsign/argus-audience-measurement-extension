#include "input/v4l2_controls.h"

#include <cstring>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

namespace v4l2ctl {

ApplyResult set_exposure_auto_priority(int fd, int want, int* before, int* actual) noexcept {
  if (before) *before = -1;
  if (actual) *actual = -1;
  if (fd < 0 || (want != 0 && want != 1)) return ApplyResult::Unsupported;

  v4l2_queryctrl qc;
  std::memset(&qc, 0, sizeof(qc));
  qc.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
  if (ioctl(fd, VIDIOC_QUERYCTRL, &qc) < 0 || (qc.flags & V4L2_CTRL_FLAG_DISABLED))
    return ApplyResult::Unsupported;

  v4l2_control c;
  std::memset(&c, 0, sizeof(c));
  c.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
  if (ioctl(fd, VIDIOC_G_CTRL, &c) == 0 && before) *before = c.value;

  std::memset(&c, 0, sizeof(c));
  c.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
  c.value = want;
  if (ioctl(fd, VIDIOC_S_CTRL, &c) < 0) return ApplyResult::Failed;

  std::memset(&c, 0, sizeof(c));
  c.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
  if (ioctl(fd, VIDIOC_G_CTRL, &c) == 0) {
    if (actual) *actual = c.value;
    return c.value == want ? ApplyResult::Applied : ApplyResult::Mismatch;
  }
  return ApplyResult::Mismatch;  // set worked but cannot be verified
}

}  // namespace v4l2ctl
