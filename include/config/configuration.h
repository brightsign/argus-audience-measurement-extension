#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "config_common.h"
#include "model_spec.h"
#include "processing_params.h"
#include "publisher_config.h"
#include "runtime_settings.h"
#include "input_factory.h"   // reuse your InputConfig (RTSP/USB/File + options)

#include <string>
#include <vector>

// Top-level application config
struct AppConfig {
  // Source
  InputConfig     input{};

  // Models
  ModelSpec       primary_model{};           // e.g., detector
  std::vector<ModelSpec> secondary_models;  // e.g., trackers, classifiers

  // Processing & runtime
  ProcessingParams processing{};
  RuntimeSettings   runtime{};

  // Outputs
  std::vector<PublisherConfig> publishers;  // support multiple sinks

  // Optional: diagnostic logging
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

