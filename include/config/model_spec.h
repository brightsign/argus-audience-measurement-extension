#ifndef MODEL_SPEC_H
#define MODEL_SPEC_H

#include <string>
#include <array>
#include "models/model_types.h"
#include "config/config_common.h"

enum class ColorLayout : uint8_t { RGB, BGR, NV12, GRAY };
enum class ChannelOrder : uint8_t { HWC, CHW };

struct Normalization {
  // If used, mean/std sizes should match channels (1, 3, or 4).
  std::array<float, 4> mean{0.f, 0.f, 0.f, 0.f};
  std::array<float, 4> std {1.f, 1.f, 1.f, 1.f};
  int channels{3};      // 1, 3, or 4
  bool to_float{true};
  float scale{1.0f/255.0f};
};

struct ModelSpec {
  // Identity / loading
  std::string name;            // "retinaface", "yolox-s"
  std::string model_path;      // "/app/models/retinaface.rknn"
  ModelFamily family{ModelFamily::Unknown};
  Backend     backend{Backend::RKNN};
  TaskType    task{TaskType::Detector};

  // Input expectations
  Size2i       input_size{320, 320};
  int          input_channels{3};
  ColorLayout  input_layout{ColorLayout::RGB};
  ChannelOrder order{ChannelOrder::HWC};
  Normalization norm{};

  // Pre/post-processing hints
  bool  keep_aspect{false};
  float conf_threshold{0.5f};
  float nms_threshold{0.45f};

  // NPU core affinity (for multi-core inference)
  // -1 = auto (runner picks best), 0/1/2 = pin to specific core
  // RK3568 (LS5/HS145) has only 1 NPU core (core 0)
  // RK3588 (XT5) has 3 NPU cores (0, 1, 2)
  int   npu_core{0};  // Default to core 0 for RK3568 compatibility

  // Basic validation; no throws
  bool validate(char* err, size_t err_sz) const noexcept;
};

#endif // MODEL_SPEC_H
