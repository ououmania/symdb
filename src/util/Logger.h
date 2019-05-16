# pragma once

#include <string>
#include <memory>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/utility/manipulators/add_value.hpp>

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

class BoostLogger {
    using LoggerType = boost::log::sources::severity_logger_mt<LogLevel>;
public:
    BoostLogger(const BoostLogger&) = delete;

    void Init(LogLevel level, const std::string &log_file);
    void Stop();

    LoggerType& Get() { return logger_; }

    static BoostLogger& Instance();

private:
    BoostLogger() = default;

    LoggerType logger_;
};

} // namespace symdb

#define LoggerInst symdb::BoostLogger::Instance()

#define LOG_WITH_LEVEL(_level) BOOST_LOG_SEV(LoggerInst.Get(), _level) \
    << boost::log::add_value("File", __FILE__) \
    << boost::log::add_value("Line", __LINE__) \
    << boost::log::add_value("Function", __FUNCTION__)

#define LOG_DEBUG LOG_WITH_LEVEL(symdb::LogLevel::DEBUG)
#define LOG_INFO LOG_WITH_LEVEL(symdb::LogLevel::INFO)
#define LOG_STATUS LOG_WITH_LEVEL(symdb::LogLevel::STATUS)
#define LOG_WARN LOG_WITH_LEVEL(symdb::LogLevel::WARNING)
#define LOG_ERROR LOG_WITH_LEVEL(symdb::LogLevel::ERROR)
#define LOG_FATAL LOG_WITH_LEVEL(symdb::LogLevel::FATAL)
