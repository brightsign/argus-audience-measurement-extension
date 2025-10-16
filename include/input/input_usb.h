#ifndef INPUT_USB_H
#define INPUT_USB_H

#include "input_source.h"
#include <memory>
#include <string>

struct UsbOptions {
  int width{640};
  int height{480};
  double fps{30.0};
  // Additional knobs: fourcc/pixel format, buffers, exposure controls...
  uint32_t v4l2_fourcc{0};   // 0 = auto
};

class UsbInputSource final : public IInputSource {
public:
  explicit UsbInputSource(std::string device, UsbOptions opts = {});
  ~UsbInputSource() override;

  InputType type() const noexcept override { return InputType::USB; }
  bool open()  noexcept override;
  bool start() noexcept override;
  void stop()  noexcept override;
  void close() noexcept override;
  FetchStatus tryFetch(FrameView& out) noexcept override;
  HealthInfo getHealth() const noexcept override;

private:
  struct Impl;               // hides V4L2/OpenCV/etc. details
  std::unique_ptr<Impl> p_;
};

#endif // INPUT_USB_H
