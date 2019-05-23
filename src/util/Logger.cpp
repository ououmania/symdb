#include "Logger.h"
#include <mutex>
#include <string>
#include <iostream>
#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/log/trivial.hpp>
#include <boost/core/null_deleter.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/sinks/frontend_requirements.hpp>

namespace logging = boost::log;
namespace attrs = boost::log::attributes;
namespace src = boost::log::sources;
namespace sinks = boost::log::sinks;
namespace expr = boost::log::expressions;
namespace keywords = boost::log::keywords;

namespace symdb {

BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", LogLevel)

std::ostream& operator<<(std::ostream& strm, LogLevel level)
{
    static std::array<const char*, (size_t) LogLevel::MAX_LEVEL> level_strings = {
        "D",
        "I",
        "W",
        "S",
        "E",
        "F"
    };

    size_t index = static_cast< std::size_t >(level);
    if (index < level_strings.size())
        strm << level_strings[index];
    else
        strm << index;

    return strm;
}

void my_formatter(logging::record_view const& rec, logging::formatting_ostream& strm)
{
    auto date_time = logging::extract< attrs::local_clock::value_type>("TimeStamp", rec);
    auto tm = boost::posix_time::to_tm(date_time.get());
    strm << '[' << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << ']';
    strm << '[' << logging::extract<LogLevel>("Severity", rec) << ']';
    auto tid = logging::extract< attrs::current_thread_id::value_type>("ThreadID", rec).get();
    strm << '[' << tid.native_id() << "]";
    auto function = logging::extract< std::string >("Function", rec);
    strm << '[' << function << "]";
    auto fullpath = logging::extract< std::string >("File", rec);
    strm << '[' << boost::filesystem::path(fullpath.get()).filename().string()
         << ':' << logging::extract<int>("Line", rec) << ']';
    strm << ' ' << rec[expr::smessage];
}

void BoostLogger::Stop()
{
    boost::log::core::get()->remove_all_sinks();
}

void BoostLogger::Init(LogLevel level, const std::string &log_file)
{
    auto core = boost::log::core::get();
    core->add_global_attribute("TimeStamp", attrs::local_clock());
    core->add_global_attribute("ThreadID", attrs::current_thread_id());

    auto backend = boost::make_shared< sinks::text_ostream_backend >();

    backend->add_stream(
        boost::shared_ptr< std::ostream >(&std::clog, boost::null_deleter()));

    // thread-safe
    typedef sinks::synchronous_sink< sinks::text_ostream_backend > text_sink;
    boost::shared_ptr<text_sink> console(new text_sink(backend));
    console->set_filter(severity >= LogLevel::STATUS);
    console->locked_backend()->auto_flush(true);
    core->add_sink(console);

    auto stream = boost::make_shared<std::ofstream>(log_file, std::ios::app);
    auto sink = boost::make_shared<text_sink>();
    sink->locked_backend()->add_stream(stream);
    sink->locked_backend()->auto_flush(true);
    sink->set_filter(severity >= level);
    sink->set_formatter(&my_formatter);
    core->add_sink(sink);
}

BoostLogger& BoostLogger::Instance()
{
    static BoostLogger logger;
    return logger;
}

} /* namespace symdb */
