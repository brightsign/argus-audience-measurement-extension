#ifndef PIPELINE_TYPES_H
#define PIPELINE_TYPES_H

#include <cstdint>
#include <vector>
#include <string>

// Reuse your Resource and Model types where included
enum class PixFmt : uint8_t { NV12, RGB24, BGR24, GRAY8 };

// Raw frame from capture (non-owning view)
struct RawFrame {
  PixFmt   fmt{PixFmt::NV12};
  int      width{0}, height{0};
  int      stride0{0}, stride1{0};
  uint8_t* plane0{nullptr};
  uint8_t* plane1{nullptr};
  int64_t  pts_ns{0};
  uint64_t seq{0}; // monotonic for debugging
};

// Preprocessed frame ready for inference
// Either points into a scratch ImageBuffer (owned by ResourceManager)
// or directly into the RKNN input tensor (zero-copy path).
struct PreprocFrame {
  // When using scratch buffer
  PixFmt   fmt{PixFmt::RGB24};
  int      width{0}, height{0};
  int      stride{0};
  uint8_t* data{nullptr};

  // When using direct tensor fill (preferred)
  uint8_t* tensor_ptr{nullptr}; // model input buffer
  int      tensor_bytes{0};

  int64_t  pts_ns{0};
  uint64_t seq{0};
};

// Minimal, model-agnostic inference outputs (non-owning view)
struct Detection {
  float x0, y0, x1, y1;
  float score;
  int   class_id{-1};
};
struct Landmarks { float pts[10]; }; // (x,y)*5
struct InferenceOut {
  const Detection* dets{nullptr}; int num_dets{0};
  const Landmarks* lms{nullptr};  int num_lms{0};
  int64_t pts_ns{0}; uint64_t seq{0};
};

// Final results after post-process (owning to survive callbacks)
struct Track {
  Detection box{};
  Landmarks lms{};
  int       id{-1};
  float     gaze_yaw{0.f};   // degrees
  float     gaze_pitch{0.f};
};
struct PipelineResult {
  std::vector<Track> tracks;
  int64_t pts_ns{0};
  uint64_t seq{0};
};

#endif // PIPELINE_TYPES_H

