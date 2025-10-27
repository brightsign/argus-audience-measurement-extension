#pragma once
#include <memory>
#include "metrics/logging.h"

// Set and get a process-wide logger (optional convenience).
void set_global_logger(std::shared_ptr<ILogger> lg) noexcept;
ILogger* get_global_logger() noexcept;

// Convenience macros
#define LG_DEBUG(FMT, ...) do{ if (auto* _lg=get_global_logger()) _lg->log(LogLevel::Debug,    FMT, ##__VA_ARGS__);}while(0)
#define LG_INFO(FMT, ...)  do{ if (auto* _lg=get_global_logger()) _lg->log(LogLevel::Info,     FMT, ##__VA_ARGS__);}while(0)
#define LG_WARN(FMT, ...)  do{ if (auto* _lg=get_global_logger()) _lg->log(LogLevel::Warn,     FMT, ##__VA_ARGS__);}while(0)
#define LG_ERROR(FMT, ...) do{ if (auto* _lg=get_global_logger()) _lg->log(LogLevel::Error,    FMT, ##__VA_ARGS__);}while(0)
#define LG_CRIT(FMT, ...)  do{ if (auto* _lg=get_global_logger()) _lg->log(LogLevel::Critical, FMT, ##__VA_ARGS__);}while(0)

