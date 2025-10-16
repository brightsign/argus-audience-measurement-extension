#ifndef LOGGING_H
#define LOGGING_H

#include <cstdint>
#include <cstdarg>
#include <string>

enum class LogLevel : uint8_t { Debug=0, Info, Warn, Error, Critical };

// Minimal interface; implement your own sink (stdout, UDP, file, etc.)
class ILogger {
public:
  virtual ~ILogger() = default;
  virtual void log(LogLevel lvl, const char* fmt, ...) noexcept = 0;
  virtual void vlog(LogLevel lvl, const char* fmt, va_list ap) noexcept = 0;
};

// No-op logger (useful as default)
class NullLogger final : public ILogger {
public:
  void log(LogLevel, const char*, ...) noexcept override {}
  void vlog(LogLevel, const char*, va_list) noexcept override {}
};

#endif // LOGGING_H

