#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "config/config_common.h"
#include "config/model_spec.h"
#include "config/processing_params.h"
#include "config/publisher_config.h"
#include "config/runtime_settings.h"
#include "input/input_factory.h"   // reuse your InputConfig (RTSP/USB/File + options)

#include <string>
#include <vector>

// Top-level application config
struct AppConfig {
  // Source
  InputConfig     input{};
  
  // Input source selection: "rtsp", "usb", or "file"
  // This allows users to configure all sources but choose which one to use
  std::string     input_source{"rtsp"};  // Default to RTSP
  
  // Input source priority: "config" (use argus-config.json) or "registry" (use BrightSign registry)
  // Priority is always: CLI args > [config or registry based on this setting] > [other] > auto-detect
  std::string     input_source_priority{"config"};  // "config" or "registry"

  // Device identification
  std::string     device_id{};  // Device identifier for MQTT (empty = auto-detect from MAC)

  // Models
  ModelSpec       primary_model{};           // e.g., detector
  std::vector<ModelSpec> secondary_models;  // e.g., trackers, classifiers

  // Processing & runtime
  ProcessingParams processing{};
  RuntimeSettings   runtime{};

  // Outputs
  std::vector<PublisherConfig> publishers;  // support multiple sinks

  // Test/Debug mode
  bool test_face_only{false};   // Enable only RetinaFace model (disable YOLOX)
  bool test_yolo_only{false};   // Enable only YOLOX model (disable RetinaFace)
  
  // Frame output options (optional)
  bool enable_frame_output{false};  // Enable decorated frame writing
  std::string output_dir{};         // Directory for decorated frames
  int max_frames{0};                // Max frames to keep (0 = no limit)
  int frame_quality{85};            // JPEG quality (1-100)

  // Face blur options (for privacy in output frames)
  bool blur_faces{false};              // Enable face blurring in output frames
  std::string blur_method{"pixelate"}; // "pixelate" or "gaussian"
  int blur_intensity{12};              // Block size (4-32) for pixelate, kernel size (31-99) for gaussian

  // Flip output frame horizontally (mirror)
  // Useful for front-facing webcams that deliver a mirrored image.
  bool flip_horizontal{false};

  // Uniform / vest classifier (MobileNetV3-Small)
  // DEPRECATED: use employee_detection below. Kept for backwards compatibility.
  bool        enable_uniform_model{false};   // Enable vest/uniform classification
  ModelSpec   uniform_model{};               // Path, npu_core, etc.

  // Employee vest detection — separate feature toggle
  // When enabled, loads a MobileNetV3 classifier to identify uniformed employees by vest.
  // Model is loaded from employee_detection.model_path (default: SD card location).
  struct EmployeeDetectionConfig {
    bool        enabled{false};                                        // Feature on/off
    std::string model_path{"/storage/sd/employee_detection/model/Mobilenetv3_small.rknn"};
    int         npu_core{2};
  } employee_detection;

  // Optional: diagnostic logging
  std::string log_level{"info"};        // "debug", "info", "warn", "error"
  std::string log_dir{"/storage/sd/logs"};
  bool        log_json{true};

  // Performance instrumentation (Method 2)
  // When true, the inference worker logs per-stage CPU/NPU timing every second:
  // resize, pre/infer/post (NPU), visualization, frame-write, and total per-frame ms.
  // Useful for diagnosing CPU load on weaker SoCs (e.g. LS5/RK3568).
  bool        log_performance{false};

  // Validate composite config (no throws). Returns true if ok.
  bool validate(char* err, size_t err_sz) const noexcept;
};

// Loader interface (implementation goes in .cpp; keep headers light)
namespace config {

// Load from a JSON/TOML/YAML file.
// Implementations should avoid exceptions and fill 'err' on failure.
// If 'strict' is false, unknown fields are ignored.
bool load_from_file(const std::string& path, AppConfig& out,
                    bool strict, char* err, size_t err_sz) noexcept;

// Save to JSON (optional; implement in .cpp if you need it)
bool save_to_file(const std::string& path, const AppConfig& in,
                  char* err, size_t err_sz) noexcept;

} // namespace config

#endif // CONFIGURATION_H

