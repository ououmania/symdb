#include "Logger.h"
#include <boost/filesystem.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/shared_ptr.hpp>
#include <fstream>
#include <string>
#include <time.h>

namespace logging = boost::log;
namespace attrs = boost::log::attributes;
namespace src = boost::log::sources;
namespace sinks = boost::log::sinks;
namespace expr = boost::log::expressions;
namespace keywords = boost::log::keywords;

namespace symdb {

BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", LogLevel)

std::ostream& operator<<(std::ostream& strm, LogLevel level) {
  static std::array<const char*, (size_t)LogLevel::MAX_LEVEL> level_strings = {
      "D", "I", "W", "S", "E", "F"};

  size_t index = static_cast<std::size_t>(level);
  if (index < level_strings.size())
    strm << level_strings[index];
  else
    strm << index;

  return strm;
}

void my_formatter(logging::record_view const& rec,
                  logging::formatting_ostream& strm) {
  auto date_time =
      logging::extract<attrs::local_clock::value_type>("TimeStamp", rec);
  auto tm = boost::posix_time::to_tm(date_time.get());
  char time_buffer[512];
  (void) strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &tm);
  strm << '[' << time_buffer << ']';
  strm << '[' << logging::extract<LogLevel>("Severity", rec) << ']';
  auto tid =
      logging::extract<attrs::current_thread_id::value_type>("ThreadID", rec)
          .get();
  strm << '[' << tid.native_id() << "]";
  auto function = logging::extract<std::string>("Function", rec);
  strm << '[' << function << "]";
  auto fullpath = logging::extract<std::string>("File", rec);
  strm << '[' << boost::filesystem::path(fullpath.get()).filename().string()
       << ':' << logging::extract<int>("Line", rec) << ']';
  strm << ' ' << rec[expr::smessage];
}

void BoostLogger::Stop() { boost::log::core::get()->remove_all_sinks(); }

void BoostLogger::Init(LogLevel level, const std::string& log_file) {
  auto core = boost::log::core::get();
  core->add_global_attribute("TimeStamp", attrs::local_clock());
  core->add_global_attribute("ThreadID", attrs::current_thread_id());

  auto console_sink = boost::log::add_console_log<char>();
  console_sink->set_filter(severity >= LogLevel::STATUS);
  console_sink->locked_backend()->auto_flush(true);

  using text_sink = sinks::synchronous_sink<sinks::text_file_backend>;

  auto log_dir = boost::filesystem::path(log_file).parent_path();
  auto log_pattern = log_dir / "file_%Y%m%d.%N.log";
  auto sink = boost::make_shared<text_sink>(
    keywords::target = log_dir,
    keywords::file_name = log_pattern,
    keywords::open_mode = std::ios_base::out | std::ios::app,
    keywords::rotation_size = 256 << 20, // 256M
    keywords::max_files = 3,
    keywords::time_based_rotation = sinks::file::rotation_at_time_interval(boost::posix_time::hours(24 * 5))
  );
  sink->locked_backend()->auto_flush(true);
  sink->set_filter(severity >= level);
  sink->set_formatter(&my_formatter);
  core->add_sink(sink);
}

BoostLogger& BoostLogger::Instance() {
  static BoostLogger logger;
  return logger;
}

} /* namespace symdb */
