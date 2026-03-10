#include "output/frame_writer.h"
#include "output/face_blur.h"
#include "pipeline/pipeline_types.h"
#include "metrics/log_global.h"
#include "util/spsc_queue.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>
#include <atomic>

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
  DiskFrameWriter(const std::string& output_dir, int max_frames, int quality,
                  const output::BlurConfig& blur_config) noexcept
    : output_dir_(output_dir), max_frames_(max_frames), quality_(quality),
      blur_config_(blur_config), frame_count_(0),
      writes_count_(0), last_log_time_(std::chrono::steady_clock::now()) {
    try {
      fs::create_directories(output_dir_);
      LG_INFO("frame_writer: created output directory %s", output_dir_.c_str());
      if (blur_config_.enabled) {
        LG_INFO("frame_writer: person blur enabled (method=%s, intensity=%d)",
                blur_config_.method == output::BlurMethod::PIXELATE ? "pixelate" : "gaussian",
                blur_config_.intensity);
      }
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
      
      // V7.2: Blur is now applied in visualization.cpp before drawing bounding boxes
      // This ensures blur stays inside box area and boxes are drawn on top
      cv::Mat output_img = img;  // No copy needed - blur already applied if enabled

      // Crop letterbox (remove black bars) if original dimensions are available
      if (result.frame_width > 0 && result.frame_height > 0 &&
          output_img.cols > 0 && output_img.rows > 0) {
        // Calculate letterbox region (same logic as resize_frame_rga)
        const float scale = std::min((float)output_img.cols / result.frame_width,
                                     (float)output_img.rows / result.frame_height);
        const int letterbox_w = (int)(result.frame_width * scale);
        const int letterbox_h = (int)(result.frame_height * scale);
        const int offset_x = (output_img.cols - letterbox_w) / 2;
        const int offset_y = (output_img.rows - letterbox_h) / 2;

        // Crop to letterbox region (removes black bars)
        if (letterbox_w > 0 && letterbox_h > 0 &&
            offset_x >= 0 && offset_y >= 0 &&
            offset_x + letterbox_w <= output_img.cols &&
            offset_y + letterbox_h <= output_img.rows) {
          cv::Rect crop_region(offset_x, offset_y, letterbox_w, letterbox_h);
          output_img = output_img(crop_region).clone();
        }
      }

      // Write frame as JPEG (data is already in correct format)
      // The RGA pipeline actually outputs BGR despite the misleading variable names
      std::vector<int> compression_params;
      compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
      compression_params.push_back(quality_);

      if (!cv::imwrite(filepath, output_img, compression_params)) {
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
  output::BlurConfig blur_config_;
  int frame_count_;
  int writes_count_;
  std::chrono::steady_clock::time_point last_log_time_;
};

// Phase 2C: Async Frame Writer
// Moves JPEG encoding and disk I/O to background thread for ~1ms per-frame gain
class AsyncDiskFrameWriter final : public IFrameWriter {
public:
  struct FrameData {
    cv::Mat image;           // Preprocessed image (cloned, blurred, cropped)
    std::string filepath;    // Target output path
    uint64_t seq;           // Frame sequence number
    int quality;            // JPEG quality
    bool valid;             // Slot in use flag
    
    FrameData() : seq(0), quality(85), valid(false) {}
  };
  
  AsyncDiskFrameWriter(const std::string& output_dir, int max_frames, int quality,
                       const output::BlurConfig& blur_config, size_t queue_size) noexcept
    : output_dir_(output_dir), max_frames_(max_frames), quality_(quality),
      blur_config_(blur_config), frame_count_(0), 
      running_(false), dropped_frames_(0), total_enqueued_(0) {
    
    // Validate queue size is power of 2
    if (queue_size == 0 || (queue_size & (queue_size - 1)) != 0) {
      LG_WARN("frame_writer_async: invalid queue_size %zu, using default 8", queue_size);
      queue_size = 8;
    }
    
    try {
      fs::create_directories(output_dir_);
      LG_INFO("frame_writer_async: created output directory %s", output_dir_.c_str());
      if (blur_config_.enabled) {
        LG_INFO("frame_writer_async: person blur enabled (method=%s, intensity=%d)",
                blur_config_.method == output::BlurMethod::PIXELATE ? "pixelate" : "gaussian",
                blur_config_.intensity);
      }
      LG_INFO("frame_writer_async: queue size=%zu (async mode enabled)", queue_size);
      
      // Start background thread
      running_.store(true, std::memory_order_release);
      writer_thread_ = std::thread(&AsyncDiskFrameWriter::writer_thread_func, this);
      
    } catch (const std::exception& e) {
      LG_ERROR("frame_writer_async: failed to initialize: %s", e.what());
    }
  }
  
  ~AsyncDiskFrameWriter() noexcept {
    // Signal stop and wait for thread
    running_.store(false, std::memory_order_release);
    
    if (writer_thread_.joinable()) {
      writer_thread_.join();
    }
    
    // Log statistics
    if (dropped_frames_ > 0) {
      int dropped = dropped_frames_.load(std::memory_order_relaxed);
      int total = total_enqueued_.load(std::memory_order_relaxed);
      float drop_rate = (total > 0) ? 
        (100.0f * dropped / (total + dropped)) : 0.0f;
      LG_WARN("frame_writer_async: shutdown - dropped %d frames (%.1f%% of total)",
              dropped, drop_rate);
    }
    int written = total_enqueued_.load(std::memory_order_relaxed);
    LG_INFO("frame_writer_async: shutdown complete (written=%d frames)", written);
  }
  
  bool writeFrame(const cv::Mat& img, const PipelineResult& result) noexcept override {
    if (img.empty()) {
      return false;
    }
    
    // Skip frames for performance (match baseline: write every 3rd frame)
    if ((frame_count_++ % 3) != 0) {
      return true;
    }
    
    try {
      // Preprocess frame in main thread (minimize time in background)
      // This includes face blur and letterbox cropping
      
      // Generate filepath
      std::string filepath;
      if (output_dir_ == "/tmp") {
        filepath = "/tmp/output.jpg";
      } else {
        auto now = std::time(nullptr);
        auto tm = std::localtime(&now);
        std::ostringstream oss;
        oss << "frame_" << std::put_time(tm, "%Y%m%d_%H%M%S") << "_" 
            << frame_count_ << ".jpg";
        filepath = (fs::path(output_dir_) / oss.str()).string();
      }
      
      // V7.2: Blur is now applied in visualization.cpp before drawing bounding boxes
      // This ensures blur stays inside box area and boxes are drawn on top
      cv::Mat output_img = img.clone();  // Clone for async processing
      
      // Crop letterbox (remove black bars) if original dimensions are available
      if (result.frame_width > 0 && result.frame_height > 0 &&
          output_img.cols > 0 && output_img.rows > 0) {
        const float scale = std::min((float)output_img.cols / result.frame_width,
                                     (float)output_img.rows / result.frame_height);
        const int letterbox_w = (int)(result.frame_width * scale);
        const int letterbox_h = (int)(result.frame_height * scale);
        const int offset_x = (output_img.cols - letterbox_w) / 2;
        const int offset_y = (output_img.rows - letterbox_h) / 2;
        
        if (letterbox_w > 0 && letterbox_h > 0 &&
            offset_x >= 0 && offset_y >= 0 &&
            offset_x + letterbox_w <= output_img.cols &&
            offset_y + letterbox_h <= output_img.rows) {
          cv::Rect crop_region(offset_x, offset_y, letterbox_w, letterbox_h);
          output_img = output_img(crop_region).clone();
        }
      }
      
      // Create frame data and enqueue
      FrameData frame_data;
      frame_data.image = std::move(output_img);
      frame_data.filepath = std::move(filepath);
      frame_data.seq = result.seq;
      frame_data.quality = quality_;
      frame_data.valid = true;
      
      // Try to enqueue (lock-free operation)
      if (!queue_.try_enqueue(std::move(frame_data))) {
        // Queue full - drop frame and log warning
        int dropped = dropped_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (dropped % 10 == 1) {  // Log every 10th drop
          LG_WARN("frame_writer_async: queue full, dropped %d frames so far", dropped);
        }
        return false;
      }
      
      total_enqueued_.fetch_add(1, std::memory_order_relaxed);
      return true;
      
    } catch (const std::exception& e) {
      LG_ERROR("frame_writer_async: exception in writeFrame: %s", e.what());
      return false;
    }
  }
  
  void flush() noexcept override {
    // Wait for queue to drain (with timeout)
    const int timeout_ms = 5000;
    const int poll_ms = 100;
    int elapsed_ms = 0;
    
    while (!queue_.empty() && elapsed_ms < timeout_ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
      elapsed_ms += poll_ms;
    }
    
    if (!queue_.empty()) {
      LG_WARN("frame_writer_async: flush timeout - %zu frames remain in queue", queue_.size());
    }
  }

private:
  void writer_thread_func() noexcept {
    LG_INFO("frame_writer_async: background writer thread started");
    
    int writes_count = 0;
    auto last_log = std::chrono::steady_clock::now();
    
    while (running_.load(std::memory_order_acquire) || !queue_.empty()) {
      FrameData frame_data;
      
      // Try to dequeue frame
      if (!queue_.try_dequeue(frame_data)) {
        // Queue empty - sleep briefly and retry
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      
      if (!frame_data.valid) {
        continue;
      }
      
      try {
        // JPEG encode and write (CPU + I/O intensive - done in background)
        std::vector<int> compression_params;
        compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
        compression_params.push_back(frame_data.quality);
        
        if (!cv::imwrite(frame_data.filepath, frame_data.image, compression_params)) {
          LG_WARN("frame_writer_async: failed to write %s", frame_data.filepath.c_str());
          continue;
        }
        
        writes_count++;
        
        // Periodic logging (every 1 second)
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
        if (elapsed_ms >= 1000) {
          float write_fps = (elapsed_ms > 0) ? (1000.0f * writes_count / elapsed_ms) : 0.0f;
          LG_INFO("frame_writer_async: performance - writes=%d fps=%.1f queue_depth=%zu",
                  writes_count, write_fps, queue_.size());
          writes_count = 0;
          last_log = now;
        }
        
        // Clean up old frames if needed
        if (max_frames_ > 0 && output_dir_ != "/tmp") {
          cleanup_old_frames();
        }
        
      } catch (const std::exception& e) {
        LG_ERROR("frame_writer_async: exception in writer thread: %s", e.what());
      }
    }
    
    LG_INFO("frame_writer_async: background writer thread stopped");
  }
  
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
          } catch (...) {}
        }
      }
    } catch (...) {}
  }
  
  std::string output_dir_;
  int max_frames_;
  int quality_;
  output::BlurConfig blur_config_;
  int frame_count_;
  
  // Async components
  SPSCQueue<FrameData, 16> queue_;  // Lock-free queue (power of 2 capacity)
  std::thread writer_thread_;
  std::atomic<bool> running_;
  std::atomic<int> dropped_frames_;
  std::atomic<int> total_enqueued_;
};

}  // namespace

std::unique_ptr<IFrameWriter> make_frame_writer_null() noexcept {
  return std::make_unique<NullFrameWriter>();
}

std::unique_ptr<IFrameWriter> make_frame_writer_disk(
    const std::string& output_dir,
    int max_frames,
    int quality,
    const output::BlurConfig& blur_config) noexcept {
  return std::make_unique<DiskFrameWriter>(output_dir, max_frames, quality, blur_config);
}

std::unique_ptr<IFrameWriter> make_frame_writer_disk_async(
    const std::string& output_dir,
    int max_frames,
    int quality,
    const output::BlurConfig& blur_config,
    size_t queue_size) noexcept {
  return std::make_unique<AsyncDiskFrameWriter>(output_dir, max_frames, quality, blur_config, queue_size);
}
