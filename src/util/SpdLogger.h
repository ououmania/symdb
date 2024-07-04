#ifndef SYMDB_UTIL_LOGGER_H
#error "Don't include this file directly"
#endif  // SYMDB_UTIL_LOGGER_H

#pragma once

#include <spdlog/spdlog.h>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace symdb {

#define VAR_WITH_LINE(a, b) a##b
#define VAR_WITH_LINE_(a, b) VAR_WITH_LINE(a, b)
#define SPD_LOGGER_VAR VAR_WITH_LINE_(spdlogger, __LINE__)
#define SPD_LOGGER_VAR_DEF(_level) \
  symdb::SpdLogger SPD_LOGGER_VAR { _level, __FILE__, __LINE__, __FUNCTION__ }

spdlog::level::level_enum LogLevelToSpdLevel(LogLevel level);
void InitLogger(LogLevel level, const std::string &log_file);

struct SpdLogContext {
  const char *file;
  const char *funcname;
  int line;
  spdlog::level::level_enum level;

  spdlog::source_loc GetSourceLoc() const {
      return spdlog::source_loc { file, line, funcname };
  }
};

class SpdLogger {
public:
  explicit SpdLogger(LogLevel level, const char *file, int line,
                     const char *funcname)
      : context_{file, funcname, line, LogLevelToSpdLevel(level)} {
    if (strcmp(file, context_.file) || line != context_.line ||
        static_cast<int>(level) != static_cast<int>(context_.level)) {
      Flush();
      current_context = context_;
    }
  }

  ~SpdLogger() {
    Flush();
  }

  template <typename T>
  SpdLogger &operator<<(const T &t) {
    current_message << t;
    return *this;
  }

  SpdLogger(const SpdLogger &) = delete;
  SpdLogger& operator=(const SpdLogger &) = delete;

private:
  void Flush() {
    if (current_message.tellp() != std::streampos{0}) {
      spdlog::default_logger_raw()->log(current_context.GetSourceLoc(),
                                        current_context.level, "{}",
                                        current_message.str());
      current_message.str("");
      current_message.clear();
    }
  }
  static thread_local std::ostringstream current_message;
  static thread_local SpdLogContext current_context;
  SpdLogContext context_;
};

}  // namespace symdb

// These macroes defined with boost-log have been used heavily. Just make life
// easy for the switch.
#define LOG_DEBUG                             \
  SPD_LOGGER_VAR_DEF(symdb::LogLevel::DEBUG); \
  SPD_LOGGER_VAR
#define LOG_INFO                             \
  SPD_LOGGER_VAR_DEF(symdb::LogLevel::INFO); \
  SPD_LOGGER_VAR
#define LOG_STATUS                             \
  SPD_LOGGER_VAR_DEF(symdb::LogLevel::STATUS); \
  SPD_LOGGER_VAR
#define LOG_WARN                                \
  SPD_LOGGER_VAR_DEF(symdb::LogLevel::WARNING); \
  SPD_LOGGER_VAR
#define LOG_ERROR                             \
  SPD_LOGGER_VAR_DEF(symdb::LogLevel::ERROR); \
  SPD_LOGGER_VAR
#define LOG_FATAL                             \
  SPD_LOGGER_VAR_DEF(symdb::LogLevel::FATAL); \
  SPD_LOGGER_VAR
