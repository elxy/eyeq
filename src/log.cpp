#include "log.hpp"

#include <iostream>

extern "C" {
#include <libavutil/log.h>
}

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/dup_filter_sink.h>

namespace EYEQ {

std::shared_ptr<spdlog::logger> Logger;

__attribute__((constructor)) static void init_logger() {
  try {
    auto dup_filter = std::make_shared<spdlog::sinks::dup_filter_sink_st>(std::chrono::seconds(5));
    dup_filter->add_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    Logger = std::make_shared<spdlog::logger>(spdlog::logger("eyeq", dup_filter));
    Logger->set_pattern("[%^%l%$] %v");
  } catch (const spdlog::spdlog_ex &ex) {
    std::cerr << "eyeq log init failed: " << ex.what() << std::endl;
  }
}

void set_log_level(LoggingLevel level) {
  // TODO: support env for libav, libplacebo log level
  av_log_set_flags(AV_LOG_SKIP_REPEATED);
  av_log_set_level(according_av_log_level(level));

  Logger->set_level(static_cast<spdlog::level::level_enum>(level));
  spdlog::set_level(static_cast<spdlog::level::level_enum>(level));
}

LoggingLevel get_log_level() { return static_cast<LoggingLevel>(Logger->level()); }

int according_av_log_level(LoggingLevel level) {
  switch (level) {
  case LoggingLevel::NONE:
    return AV_LOG_QUIET;
  case LoggingLevel::FATAL:
  case LoggingLevel::ERR:
    return AV_LOG_FATAL;
  case LoggingLevel::WARN:
  case LoggingLevel::INFO:
    return AV_LOG_WARNING;
  case LoggingLevel::DEBUG:
    return AV_LOG_INFO;
  case LoggingLevel::TRACE:
    return AV_LOG_DEBUG;
  default:
    throw std::logic_error("Invalid log level");
  }
}

enum pl_log_level according_pl_log_level(LoggingLevel level) {
  switch (level) {
  case LoggingLevel::NONE:
    return PL_LOG_NONE;
  case LoggingLevel::FATAL:
  case LoggingLevel::ERR:
    return PL_LOG_FATAL;
  case LoggingLevel::WARN:
    return PL_LOG_ERR;
  case LoggingLevel::INFO:
    return PL_LOG_WARN;
  case LoggingLevel::DEBUG:
    return PL_LOG_INFO;
  case LoggingLevel::TRACE:
    return PL_LOG_DEBUG;
  default:
    throw std::logic_error("Invalid log level");
  }
}

pl_log create_pl_log(LoggingLevel level) {
  enum pl_log_level pllvl = according_pl_log_level(level);
  struct pl_log_params plparams = {.log_cb = pl_log_color, .log_level = pllvl};
  return pl_log_create(PL_API_VER, &plparams);
}

}; // namespace EYEQ
