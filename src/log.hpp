#pragma once

#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <libplacebo/log.h>

namespace EYEQ {

extern std::shared_ptr<spdlog::logger> Logger;

enum class LoggingLevel {
  NONE = spdlog::level::off,
  FATAL = spdlog::level::critical,
  ERR = spdlog::level::err,
  WARN = spdlog::level::warn,
  INFO = spdlog::level::info,
  DEBUG = spdlog::level::debug,
  TRACE = spdlog::level::trace,
};

void set_log_level(LoggingLevel level);

LoggingLevel get_log_level();

int according_av_log_level(LoggingLevel level);

enum pl_log_level according_pl_log_level(LoggingLevel level);

pl_log create_pl_log(LoggingLevel level);

}; // namespace EYEQ
