#include "Logger.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/pattern_formatter.h>

namespace symdb {

thread_local std::ostringstream SpdLogger::current_message;
thread_local SpdLogContext SpdLogger::current_context;

spdlog::level::level_enum LogLevelToSpdLevel(LogLevel level) {
  switch (level) {
    case LogLevel::DEBUG:
      return spdlog::level::debug;
    case LogLevel::INFO:
    case LogLevel::STATUS:
      return spdlog::level::info;
    case LogLevel::ERROR:
      return spdlog::level::err;
    case LogLevel::WARNING:
      return spdlog::level::warn;
    case LogLevel::FATAL:
      return spdlog::level::critical;
    default:
      return spdlog::level::debug;
  }
}

void InitLogger(LogLevel level, const std::string& log_file) {
  auto spdlog_level = LogLevelToSpdLevel(level);
  auto new_logger = spdlog::rotating_logger_mt(
      "file_logger", log_file, kMaxFileSize, kMaxFileCount, true);
  new_logger->set_level(spdlog_level);
  // The flush_every function in the official wiki is not implemented.
  new_logger->flush_on(spdlog_level);
  // %@: source file location
  // %!: source function
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%f][%L][%t][%@][%!] %v");
  spdlog::set_default_logger(new_logger);
}

} // namespace symdb
