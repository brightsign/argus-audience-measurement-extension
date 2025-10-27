#include "input/input_factory.h"
#include "input/input_usb.h"
#include "input/input_rtsp.h"
#include "input/input_file.h"

#if 0
std::unique_ptr<IInputSource> make_input(const InputConfig& ic) {
  if (!ic.rtsp_url.empty()) {
    return std::make_unique<RtspInputSource>(ic.rtsp_url, ic.rtsp);
  }
  if (!ic.usb_device.empty()) {
    return std::make_unique<UsbInputSource>(ic.usb_device, ic.usb);
  }
  if (!ic.file_path.empty()) {
    return std::make_unique<FileInputSource>(ic.file_path, ic.file);
  }
  return nullptr; // or a NullInput that always returns NoFrame
}
#endif