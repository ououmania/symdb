#ifndef SYMDB_UTIL_LOGGER_H
#define SYMDB_UTIL_LOGGER_H

#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/utility/manipulators/add_value.hpp>
#include <memory>
#include <string>

namespace symdb {

enum class LogLevel {
  DEBUG,
  INFO,
  WARNING,
  STATUS,
  ERROR,
  FATAL,
  MAX_LEVEL,
};

struct LogConfig {
  std::string file_path;
  LogLevel level;
};

constexpr int kMaxFileSize = 128 << 20;
constexpr int kMaxFileCount = 3;

}  // namespace symdb

#ifdef USE_BOOST_LOG
#include "BoostLogger.h"
#else
#include "SpdLogger.h"
#endif

#endif // SYMDB_UTIL_LOGGER_H
