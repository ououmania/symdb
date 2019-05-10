#include "Config.h"
#include "pugixml.hpp"
#include "util/Logger.h"
#include "util/Functions.h"
#include "util/Exceptions.h"
#include "util/NetDefine.h"
#include <iostream>
#include <exception>
#include <boost/filesystem.hpp>
#include <cstdio>

namespace symdb
{

static const std::string kProtoGeneratedFilePattern = ".*\\.pb\\.(cc|h)$";

inline std::string child_value_or_default(
    const pugi::xml_node &node,
    const char *child_name,
    const std::string &def)
{
    const pugi::xml_node &child = node.child(child_name);
    if (child) {
        return child.child_value();
    } else {
        return def;
    }
}

inline std::string child_value_or_throw(
    const pugi::xml_node &node,
    const char *child_name)
{
    const pugi::xml_node &child = node.child(child_name);
    if (!child) {
        THROW_AT_FILE_LINE("node<%s> has no child<%s>",
                           node.name(), child_name);
    }
    return child.child_value();
}

RegexPattern::RegexPattern(const std::string &orig_pattern,
                 const std::string &use_pattern)
    : pattern { orig_pattern },
      reg { std::regex(use_pattern) },
      is_copied_from_global { false }
{
}

RegexPattern::RegexPattern(const std::string &the_pattern)
    : RegexPattern(the_pattern, the_pattern)
{
}

void Config::Init(const std::string &xml_file)
{
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(xml_file.c_str());
    if (!result) {
        THROW_AT_FILE_LINE("failed to load_file %s: %s", xml_file.c_str(),
                           result.description());
    }

    auto root_node = doc.child("Config");
    log_path_ = child_value_or_throw(root_node, "LogDir");
    db_path_ = child_value_or_throw(root_node, "DataDir");
    listen_path_ = child_value_or_default(root_node, "Listen", symdb::kDefaultSockPath);

    auto ensure_dir_exists = [](const std::string &dir) {
        boost::filesystem::path dir_path(dir);
        boost::filesystem::create_directories(dir_path);
    };

    ensure_dir_exists(db_path_);
    ensure_dir_exists(log_path_);

    boost::filesystem::path log_file = boost::filesystem::path(log_path_) / "symdb.log";

    LoggerInst.Init(symdb::LogLevel::DEBUG, log_file.string());

    InitGlobalExcludePattern(root_node);
    InitProjectsConfig(root_node);
    InitDefaultIncDirs(root_node);
}

void Config::InitGlobalExcludePattern(const pugi::xml_node &root_node)
{
    global_excluded_patterns_.push_back(RegexPattern(kProtoGeneratedFilePattern));
    global_excluded_patterns_.push_back(RegexPattern(".*/build/.*"));

    auto global_exclude_entry = root_node.select_nodes("//GlobalExcluded/ExcludeEntry");
    for (const auto &entry : global_exclude_entry) {
        std::string pattern = entry.node().attribute("pattern").as_string();
        if (pattern.find("{PROJECT_HOME}") != std::string::npos) {
            global_project_patterns_.push_back(pattern);
        } else {
            global_excluded_patterns_.push_back(RegexPattern {pattern});
        }
    }
}

void Config::InitProjectsConfig(const pugi::xml_node &root)
{
    project_config_.clear();

    auto project_nodes = root.select_nodes("//Projects/Project");
    project_config_.reserve(project_nodes.size());

    for (const auto &proj : project_nodes) {
        const auto &node = proj.node();
        ProjectConfigPtr pc = std::make_shared<ProjectConfig>();
        pc->name = child_value_or_throw(node, "Name");
        pc->home_path = child_value_or_throw(node, "Home");
        for (const auto &entry : node.children("ExcludeEntry")) {
            std::string cfg_pattern = entry.attribute("pattern").as_string();
            std::string used_pattern = cfg_pattern;
            symutil::replace_string(used_pattern, "{PROJECT_HOME}", pc->home_path);
            pc->exclude_patterns.push_back(RegexPattern { cfg_pattern, used_pattern });
        }

        for (const auto &pattern : global_project_patterns_) {
            std::string used_pattern = pattern;
            symutil::replace_string(used_pattern, "{PROJECT_HOME}", pc->home_path);
            if (pattern == used_pattern) {
                LOG_ERROR << "no project info in pattern: " << pattern;
                continue;
            }
            RegexPattern rp { pattern, used_pattern };
            rp.is_copied_from_global = true;
            pc->exclude_patterns.push_back(rp);
        }
        project_config_.push_back(pc);
    }
}

void Config::InitDefaultIncDirs(const pugi::xml_node &root)
{
    default_inc_dirs_.clear();

    auto dir_xpath = root.select_nodes("//SystemInclude/Directory");
    if (!dir_xpath.empty()) {
        default_inc_dirs_.reserve(dir_xpath.size() * 2);
        for (const auto &x : dir_xpath) {
            default_inc_dirs_.push_back("-isystem");
            default_inc_dirs_.push_back(x.node().child_value());
        }
    } else {
        default_inc_dirs_ = {
            "-isystem", "/usr/local/include/",
            "-isystem", "/usr/include/c++/8.3.0",
            "-isystem", "/usr/include/c++/8.3.0/x86_64-pc-linux-gnu",
            "-isystem", "/usr/include/",
            "-isystem", "/usr/lib/gcc/x86_64-pc-linux-gnu/8.3.0/include-fixed",
            "-isystem", "/usr/lib/gcc/x86_64-pc-linux-gnu/8.3.0/include/",
        };
    }


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

bool Config::IsFileExcluded(const fspath &path) const {
    for (const auto &rp : global_excluded_patterns_) {
        bool is_matched = std::regex_match(path.string(), rp.reg);
        LOG_DEBUG << "abs_path=" << path << " pattern=" << rp.pattern
                  << " is_matched=" << is_matched;
        if (is_matched) {
            return true;
        }
    }

    return false;
}

} /* symdb */
