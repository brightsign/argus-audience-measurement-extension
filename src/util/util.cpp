#include "util/util.h"
#include "metrics/log_global.h"

#include <sys/stat.h>
#include <libgen.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
inline bool is_regular(const struct stat& st) { return S_ISREG(st.st_mode); }
inline bool is_dir(const struct stat& st)     { return S_ISDIR(st.st_mode); }
}

const char* get_opt(const char* flag, int argc, char** argv) noexcept {
  if (!flag) return nullptr;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == flag) return argv[i+1];
  }
  return nullptr;
}

bool file_exists(const char* path) noexcept {
  if (!path || !*path) return false;
  struct stat st{};
  return ::stat(path, &st) == 0 && is_regular(st);
}

bool dir_exists(const char* path) noexcept {
  if (!path || !*path) return false;
  struct stat st{};
  return ::stat(path, &st) == 0 && is_dir(st);
}

bool ensure_dir(const char* path) noexcept {
  if (dir_exists(path)) return true;
  return ::mkdir(path, 0755) == 0;
}

std::string join_path(const std::string& a, const std::string& b) noexcept {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

std::string dirname_of_exe(const char* argv0) noexcept {
  if (!argv0) return {};
  char buf[1024];
  std::snprintf(buf, sizeof(buf), "%s", argv0);
  char* d = ::dirname(buf);
  return d ? std::string(d) : std::string{};
}

static std::string choose_local_sample(const std::string& bin_dir) {
  return join_path(join_path(bin_dir, "configs"), "argus-config.json");
}

std::string pick_config_path(int argc, char** argv) noexcept {
  LG_INFO("[pick_config_path] CLI argument (highest priority)");
  // 1) CLI argument (highest priority)
  if (const char* cli = get_opt("--config", argc, argv)) {
    LG_INFO("[pick_config_path] Check file existsence for CLI config: %s", cli);
    if (file_exists(cli)) {
      LG_INFO("[pick_config_path] Using CLI config: %s", cli);
      return std::string(cli);
    }
  }
  
  // 2) Environment variable
  if (const char* env = std::getenv("BSEXT_CONFIG")) {
    if (file_exists(env)) {
      LG_INFO("[pick_config_path] Using ENV config: %s", env);
      return std::string(env);
    }
  }
  
  // 3) Writable SD card location (user override, preferred)
  
  static const char* kSdConfig = "/storage/sd/configs/argus-config.json";
  LG_INFO("[pick_config_path] Using SD config: %s", kSdConfig);
  bool sd_exists = file_exists(kSdConfig);
  LG_INFO("[pick_config_path] SD config check: %s -> %s", 
          kSdConfig, sd_exists ? "EXISTS" : "NOT FOUND");
  if (sd_exists) {
    LG_INFO("[pick_config_path] Using SD config: %s", kSdConfig);
    return std::string(kSdConfig);
  }
  
  // 4) Next to binary (read-only /var location - default from package)
  const std::string bin_dir = dirname_of_exe(argv ? argv[0] : nullptr);
  const std::string local   = choose_local_sample(bin_dir);
  bool local_exists = file_exists(local.c_str());
  LG_INFO("[pick_config_path] Package config check: %s -> %s",
          local.c_str(), local_exists ? "EXISTS" : "NOT FOUND");
  if (local_exists) {
    LG_INFO("[pick_config_path] Using package config: %s", local.c_str());
    return local;
  }

  // Fallback (may not exist; caller will log a warning)
  LG_WARN("[pick_config_path] Fallback to: %s", local.c_str());
  return local;
}

bool ensure_device_config_present(const char* src_sample_json, const char* dst_dir) noexcept {
  if (!dst_dir || !*dst_dir) return false;
  if (!dir_exists(dst_dir))  { if (!ensure_dir(dst_dir)) return false; }

  const std::string dst = join_path(dst_dir, "argus-config.json");
  if (file_exists(dst.c_str())) return true;         // already there

  if (!src_sample_json || !file_exists(src_sample_json)) return file_exists(dst.c_str());

  // Copy src -> dst
  std::FILE* in  = std::fopen(src_sample_json, "rb");
  std::FILE* out = std::fopen(dst.c_str(), "wb");
  if (!in || !out) {
    if (in) std::fclose(in);
    if (out) std::fclose(out);
    return file_exists(dst.c_str());
  }
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
    if (std::fwrite(buf, 1, n, out) != n) { break; }
  }
  std::fclose(in);
  std::fclose(out);
  return file_exists(dst.c_str());
}

