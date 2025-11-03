#ifndef FRAME_WRITER_H
#define FRAME_WRITER_H

#include <memory>
#include <string>
#include <opencv2/core.hpp>

// Forward declarations
struct PipelineResult;
struct ImageBuffer;
struct FrameView;
struct InferenceOutputs;

/**
 * Frame writer: Writes decorated/annotated frames to disk or RTSP output.
 * 
 * Usage pattern:
 *   auto writer = make_frame_writer_disk("/storage/sd/decorated");
 *   writer->writeFrame(img, result);  // writes decorated frame
 */
class IFrameWriter {
public:
  virtual ~IFrameWriter() = default;

  // Write a single decorated frame
  // img: cv::Mat with BGR/NV12/RGB data
  // result: inference detections/landmarks
  // Returns false on write failure, true otherwise
  virtual bool writeFrame(const cv::Mat& img, const PipelineResult& result) noexcept = 0;
  
  // Optional: write frame with raw InferenceOutputs (for debugging)
  virtual bool writeFrameRaw(const cv::Mat& img, const InferenceOutputs& outs) noexcept {
    return true;  // default no-op
  }

  // Optional: flush pending writes
  virtual void flush() noexcept {}
};

// Create a frame writer that saves decorated frames to disk (JPEG)
// output_dir: directory to save frames (will be created if needed)
// max_frames: max frames to keep (0 = unlimited)
// quality: JPEG quality (0-100)
std::unique_ptr<IFrameWriter> make_frame_writer_disk(
    const std::string& output_dir,
    int max_frames = 0,
    int quality = 85) noexcept;

// Create a no-op frame writer (useful for disabling frame output)
std::unique_ptr<IFrameWriter> make_frame_writer_null() noexcept;

#endif // FRAME_WRITER_H
