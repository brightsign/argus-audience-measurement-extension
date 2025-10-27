#include "metrics/log_global.h"
#include <atomic>

static std::shared_ptr<ILogger> g_logger;
void set_global_logger(std::shared_ptr<ILogger> lg) noexcept { g_logger = std::move(lg); }
ILogger* get_global_logger() noexcept { return g_logger.get(); }

