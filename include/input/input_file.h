#ifndef INPUT_FILE_H
#define INPUT_FILE_H

#include "input_source.h"
#include <memory>
#include <string>

struct FileOptions {
  bool loop{false};
  double max_fps{30.0};      // pacing for decode; 0 = as fast as possible
  bool decode_to_nv12{true}; // prefer NV12 for zero-copy into RGA
};

class FileInputSource final : public IInputSource {
public:
  explicit FileInputSource(std::string path, FileOptions opts = {});
  ~FileInputSource() override;

  InputType type() const noexcept override { return InputType::File; }
  bool open()  noexcept override;
  bool start() noexcept override;
  void stop()  noexcept override;
  void close() noexcept override;
  FetchStatus tryFetch(FrameView& out) noexcept override;
  HealthInfo getHealth() const noexcept override;

private:
  struct Impl;               // hides demux/decoder (e.g., OpenCV/GStreamer)
  std::unique_ptr<Impl> p_;
};

#endif // INPUT_FILE_H
