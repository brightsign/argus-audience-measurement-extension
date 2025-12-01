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
  
  // Input source priority: "config" (use config.json) or "registry" (use BrightSign registry)
  // Priority is always: CLI args > [config or registry based on this setting] > [other] > auto-detect
  std::string     input_source_priority{"config"};  // "config" or "registry"

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
  
  // Optional: diagnostic logging
  std::string log_level{"info"};        // "debug", "info", "warn", "error"
  std::string log_dir{"/storage/sd/logs"};
  bool        log_json{true};

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

