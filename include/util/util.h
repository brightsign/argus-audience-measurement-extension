#pragma once
#include <string>

// ---- CLI helpers ----

// Return the value that follows a flag (e.g. --config <path>), or nullptr if not present.
// Example: const char* cfg = get_opt("--config", argc, argv);
const char* get_opt(const char* flag, int argc, char** argv) noexcept;

// Choose a config file path using this order:
//   1) --config <path> (CLI argument)
//   2) $BSEXT_CONFIG (environment variable)
//   3) /storage/sd/configs/argus-config.json (writable user override - preferred)
//   4) <binary_dir>/configs/argus-config.json (read-only package default)
// Returns a string (may point to a non-existent path; call file_exists to verify).
std::string pick_config_path(int argc, char** argv) noexcept;


// ---- Filesystem helpers ----

// True if file exists and is a regular file.
bool file_exists(const char* path) noexcept;

// True if a directory exists.
bool dir_exists(const char* path) noexcept;

// Ensure a directory exists (creates it if missing, 0755). Returns true on success or already exists.
bool ensure_dir(const char* path) noexcept;

// Join two path segments with one slash. (No normalization beyond that.)
std::string join_path(const std::string& a, const std::string& b) noexcept;

// Return the directory that contains the running executable.
std::string dirname_of_exe(const char* argv0) noexcept;


// ---- Convenience for shipping default configs ----

// If dst_dir/argus-config.json is missing and src exists, copy src -> dst_dir/argus-config.json.
// Returns true if the destination exists after the call (either already existed or copied successfully).
bool ensure_device_config_present(const char* src_sample_json, const char* dst_dir) noexcept;


