#include "output/frame_writer.h"
#include "pipeline/pipeline_types.h"
#include "metrics/log_global.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#define ENABLE_DEBUG
namespace fs = std::filesystem;

namespace {

class NullFrameWriter final : public IFrameWriter {
public:
  bool writeFrame(const cv::Mat&, const PipelineResult&) noexcept override {
    return true;
  }
};

class DiskFrameWriter final : public IFrameWriter {
public:
  DiskFrameWriter(const std::string& output_dir, int max_frames, int quality) noexcept
    : output_dir_(output_dir), max_frames_(max_frames), quality_(quality), frame_count_(0),
      writes_count_(0), last_log_time_(std::chrono::steady_clock::now()) {
    try {
      fs::create_directories(output_dir_);
      LG_INFO("frame_writer: created output directory %s", output_dir_.c_str());
    } catch (const std::exception& e) {
      LG_ERROR("frame_writer: failed to create output directory: %s", e.what());
    }
  }

  bool writeFrame(const cv::Mat& img, const PipelineResult& result) noexcept override {
    if (img.empty()) {
      LG_WARN("frame_writer: empty image, skipping");
      return false;
    }

    // Skip most frames for performance (CPU optimization: reduce I/O)
    // Only write every 3rd frame to match working_1 baseline (10 FPS output)
    if ((frame_count_++ % 3) != 0) {
      return true;
    }

    try {
      // For /tmp output, write directly to fixed filename (overwrite mode)
      std::string filepath;
      if (output_dir_ == "/tmp") {
        filepath = "/tmp/output.jpg";
      } else {
        // Generate timestamp-based filename for other directories
        auto now = std::time(nullptr);
        auto tm = std::localtime(&now);
        std::ostringstream oss;
        oss << "frame_" << std::put_time(tm, "%Y%m%d_%H%M%S") << "_" 
            << (frame_count_++) << ".jpg";
        filepath = (fs::path(output_dir_) / oss.str()).string();
      }
      
      // Write frame as JPEG (data is already in correct format)
      // The RGA pipeline actually outputs BGR despite the misleading variable names
      std::vector<int> compression_params;
      compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
      compression_params.push_back(quality_);

      if (!cv::imwrite(filepath, img, compression_params)) {
        LG_WARN("frame_writer: failed to write frame to %s", filepath.c_str());
        return false;
      }
      #ifdef ENABLE_DEBUG
      // Track write count and log performance metrics every 1 second
      writes_count_++;
      auto now = std::chrono::steady_clock::now();
      auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time_).count();
      
      if (elapsed_ms >= 1000) {
        float write_fps = (elapsed_ms > 0) ? (1000.0f * writes_count_ / elapsed_ms) : 0.0f;
        LG_INFO("frame_writer: performance metrics (writes=%d, fps=%.1f, skip_ratio=1:3)",
                writes_count_, write_fps);
        writes_count_ = 0;
        last_log_time_ = now;
      }

      
      LG_DEBUG("frame_writer: wrote %s (seq=%llu tracks=%zu)",
               filepath.c_str(), (unsigned long long)result.seq, 
               (size_t)result.tracks.size());
      #endif

      // Clean up old frames if max_frames limit exceeded (only for timestamped mode)
      if (max_frames_ > 0 && output_dir_ != "/tmp") {
        cleanup_old_frames();
      }

      return true;
    } catch (const std::exception& e) {
      LG_ERROR("frame_writer: exception while writing frame: %s", e.what());
      return false;
    }
  }

private:
  void cleanup_old_frames() noexcept {
    try {
      std::vector<fs::path> frames;
      for (const auto& entry : fs::directory_iterator(output_dir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
          frames.push_back(entry.path());
        }
      }

      if ((int)frames.size() > max_frames_) {
        std::sort(frames.begin(), frames.end(),
                  [](const fs::path& a, const fs::path& b) {
                    return fs::last_write_time(a) < fs::last_write_time(b);
                  });

        int to_delete = (int)frames.size() - max_frames_;
        for (int i = 0; i < to_delete; ++i) {
          try {
            fs::remove(frames[i]);
            LG_DEBUG("frame_writer: deleted old frame %s", frames[i].string().c_str());
          } catch (const std::exception& e) {
            LG_WARN("frame_writer: failed to delete %s: %s", 
                    frames[i].string().c_str(), e.what());
          }
        }
      }
    } catch (const std::exception& e) {
      LG_WARN("frame_writer: error during cleanup: %s", e.what());
    }
  }

  std::string output_dir_;
  int max_frames_;
  int quality_;
  int frame_count_;
  int writes_count_;
  std::chrono::steady_clock::time_point last_log_time_;
};

}  // namespace

std::unique_ptr<IFrameWriter> make_frame_writer_null() noexcept {
  return std::make_unique<NullFrameWriter>();
}

std::unique_ptr<IFrameWriter> make_frame_writer_disk(
    const std::string& output_dir,
    int max_frames,
    int quality) noexcept {
  return std::make_unique<DiskFrameWriter>(output_dir, max_frames, quality);
}
