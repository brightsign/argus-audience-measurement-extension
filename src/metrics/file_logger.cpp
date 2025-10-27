#include "metrics/file_logger.h"
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool ensure_dir(const char* dir) {
  struct stat st{};
  if (::stat(dir, &st) == 0) return S_ISDIR(st.st_mode);
  return ::mkdir(dir, 0755) == 0;
}
}

FileRotatingLogger::FileRotatingLogger() noexcept
: FileRotatingLogger(Config{}) {}

FileRotatingLogger::FileRotatingLogger(const Config& cfg) noexcept
: path_(cfg.path),
  max_bytes_(cfg.max_mb ? cfg.max_mb*1024u*1024u : max_bytes_),
  max_files_(cfg.max_files > 1 ? cfg.max_files : max_files_),
  min_level_(cfg.min_level),
  also_stderr_(cfg.also_stderr),
  utc_time_(cfg.utc_time) {
  std::lock_guard<std::mutex> lg(mu_);
  if (path_.rfind("/storage/sd/",0)==0) ensure_dir("/storage/sd/logs");
  if (!open_file_unlocked(path_)) {
    path_ = "/tmp/gaze.log";
    open_file_unlocked(path_);
  }
}

FileRotatingLogger::~FileRotatingLogger() {
  std::lock_guard<std::mutex> lg(mu_);
  if (fp_) std::fclose(fp_);
}

void FileRotatingLogger::set_level(LogLevel lvl) noexcept {
  std::lock_guard<std::mutex> lg(mu_);
  min_level_ = lvl;
}

void FileRotatingLogger::log(LogLevel lvl, const char* fmt, ...) noexcept {
  va_list ap; va_start(ap, fmt);
  vlog(lvl, fmt, ap);
  va_end(ap);
}

void FileRotatingLogger::vlog(LogLevel lvl, const char* fmt, va_list ap) noexcept {
  if (lvl < min_level_) return;
  std::lock_guard<std::mutex> lg(mu_);
  vwrite(lvl, fmt, ap);
}

void FileRotatingLogger::vwrite(LogLevel lvl, const char* fmt, va_list ap) noexcept {
  // timestamp
  using clk = std::chrono::system_clock;
  auto now = clk::now();
  auto sec = std::chrono::time_point_cast<std::chrono::seconds>(now);
  auto us  = std::chrono::duration_cast<std::chrono::microseconds>(now - sec).count();
  std::time_t tt = clk::to_time_t(now);
  std::tm tm{};
  if (utc_time_) gmtime_r(&tt, &tm);
  else           localtime_r(&tt, &tm);

  char prefix[96];
  std::snprintf(prefix, sizeof(prefix), "%04d-%02d-%02d %02d:%02d:%02d.%06ld [%s] ",
    tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
    tm.tm_hour, tm.tm_min, tm.tm_sec, (long)us, level_str(lvl));

  char msg[2048];
  va_list ap2; va_copy(ap2, ap);
  std::vsnprintf(msg, sizeof(msg), fmt, ap2);
  va_end(ap2);

  // stderr
  if (also_stderr_) {
    std::fwrite(prefix, 1, std::strlen(prefix), stderr);
    std::fwrite(msg,    1, std::strlen(msg),    stderr);
    std::fwrite("\n",   1, 1,                   stderr);
    std::fflush(stderr);
  }

  // file
  if (fp_) {
    rotate_if_needed_unlocked();
    std::fwrite(prefix, 1, std::strlen(prefix), fp_);
    std::fwrite(msg,    1, std::strlen(msg),    fp_);
    std::fwrite("\n",   1, 1,                   fp_);
    std::fflush(fp_);
  }
}

void FileRotatingLogger::rotate_if_needed_unlocked() noexcept {
  if (!fp_) return;
  long pos = std::ftell(fp_);
  if (pos >= 0 && static_cast<size_t>(pos) < max_bytes_) return;

  std::fclose(fp_); fp_ = nullptr;

  // gaze.log.N <- ... <- gaze.log.1 <- gaze.log
  for (int i = max_files_-1; i >= 1; --i) {
    char src[512], dst[512];
    std::snprintf(src, sizeof(src), "%s.%d", path_.c_str(), i);
    std::snprintf(dst, sizeof(dst), "%s.%d", path_.c_str(), i+1);
    ::rename(src, dst);
  }
  char first[512];
  std::snprintf(first, sizeof(first), "%s.1", path_.c_str());
  ::rename(path_.c_str(), first);

  open_file_unlocked(path_);
}

bool FileRotatingLogger::open_file_unlocked(const std::string& prefer) noexcept {
  fp_ = std::fopen(prefer.c_str(), "a");
  return fp_ != nullptr;
}

const char* FileRotatingLogger::level_str(LogLevel l) noexcept {
  switch (l) {
    case LogLevel::Debug:    return "DBG";
    case LogLevel::Info:     return "INF";
    case LogLevel::Warn:     return "WRN";
    case LogLevel::Error:    return "ERR";
    case LogLevel::Critical: return "CRT";
    default:                 return "UNK";
  }
}

