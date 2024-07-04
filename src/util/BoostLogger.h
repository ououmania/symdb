#ifndef SYMDB_UTIL_LOGGER_H
#error "Don't include this file directly"
#endif // SYMDB_UTIL_LOGGER_H

#pragma once

#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/utility/manipulators/add_value.hpp>
#include <memory>
#include <string>

namespace symdb {

class BoostLogger {
  using LoggerType = boost::log::sources::severity_logger_mt<LogLevel>;

public:
  BoostLogger(const BoostLogger&) = delete;

  void Init(LogLevel level, const std::string& log_file);
  void Stop();

  LoggerType& Get() { return logger_; }

  static BoostLogger& Instance();

private:
  BoostLogger() = default;

  LoggerType logger_;
};

inline void InitLogger(LogLevel level, const std::string& log_file) {
    BoostLogger::Instance().Init(level, log_file);
}

}  // namespace symdb

#define BOOST_LOG_WITH_LEVEL(_level)             \
  BOOST_LOG_SEV(LoggerInst.Get(), _level)        \
      << boost::log::add_value("File", __FILE__) \
      << boost::log::add_value("Line", __LINE__) \
      << boost::log::add_value("Function", __FUNCTION__)

#define LOG_DEBUG BOOST_LOG_WITH_LEVEL(symdb::LogLevel::DEBUG)
#define LOG_INFO BOOST_LOG_WITH_LEVEL(symdb::LogLevel::INFO)
#define LOG_STATUS BOOST_LOG_WITH_LEVEL(symdb::LogLevel::STATUS)
#define LOG_WARN BOOST_LOG_WITH_LEVEL(symdb::LogLevel::WARNING)
#define LOG_ERROR BOOST_LOG_WITH_LEVEL(symdb::LogLevel::ERROR)
#define LOG_FATAL BOOST_LOG_WITH_LEVEL(symdb::LogLevel::FATAL)
