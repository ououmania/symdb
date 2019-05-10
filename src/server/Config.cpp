#include "Config.h"
#include "util/Logger.h"
#include "util/Exceptions.h"
#include <iostream>
#include <exception>
#include <boost/filesystem.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/algorithm/string.hpp>
#include <cstdio>

namespace symdb
{

static const std::string kProtoGeneratedFilePattern = ".*\\.pb\\.(cc|h)$";

void Config::Init(const std::string &xml_file)
{
    using boost::property_tree::ptree;

    ptree pt;
    read_xml(xml_file, pt);
    log_path_ = pt.get<std::string>("Config.LogDir");
    db_path_ = pt.get<std::string>("Config.DataDir");
    boost::filesystem::path log_bpath(log_path_);
    boost::filesystem::create_directories(log_bpath);
    boost::filesystem::path db_bpath(db_path_);
    boost::filesystem::create_directories(db_bpath);
    boost::filesystem::path log_file = log_bpath / "symdb.log";

    LoggerInst.Init(symdb::LogLevel::DEBUG, log_file.string());

    InitDefaultIncDirs();

    excluded_file_patterns_.push_back(std::regex(kProtoGeneratedFilePattern));
    excluded_file_patterns_.push_back(std::regex(".*/test/.*"));
}

bool Config::IsFileExcluded(const fspath &path) const {
    for (const auto &r : excluded_file_patterns_) {
        bool is_matched = std::regex_match(path.string(), r);
        LOG_DEBUG << "abs_path=" << path << ", is_matched=" << is_matched;
        if (is_matched) {
            return true;
        }
    }

    return false;
}

void Config::InitDefaultIncDirs()
{
    default_inc_dirs_ = {
        "-isystem", "/usr/local/include/",
        "-isystem", "/usr/include/c++/8.3.0",
        "-isystem", "/usr/include/c++/8.3.0/x86_64-pc-linux-gnu",
        "-isystem", "/usr/include/",
        "-isystem", "/usr/lib/gcc/x86_64-pc-linux-gnu/8.3.0/include-fixed",
        "-isystem", "/usr/lib/gcc/x86_64-pc-linux-gnu/8.3.0/include/",
    };

    // gdb doesn't work well with popen

    /*const std::string command = "g++ -E -x c++ - -v < /dev/null 2>&1";
    const std::string kSysIncSearchBegin = "#include <...> search starts here:";
    const std::string kSysIncSearchEnd = "End of search list.";
    FILE *stream = popen(command.c_str(), "r");
    if (stream == nullptr) {
        THROW_EXCEPTION_AT_FILE_LINE("popen error: %s", strerror(errno));
    }
    std::vector<std::string> default_inc_dirs;
    bool is_started = false;
    char line[4096];
    while (fgets(line, 4096, stream) != nullptr) {
        std::istringstream iss { line };
        std::string str_line;
        std::getline(iss, str_line, '\n');
        boost::trim(str_line);
        if (is_started && str_line.find(kSysIncSearchEnd) != std::string::npos) {
            break;
        } else if (is_started) {
            LOG_DEBUG << "Add default inc dir: " << str_line;
            default_inc_dirs_.push_back("-isystem");
            default_inc_dirs_.push_back(str_line);
        } else if (strstr(line, kSysIncSearchBegin.c_str()) != nullptr) {
            is_started = true;
        }
    }
    pclose(stream);*/
}

} /* symdb */
