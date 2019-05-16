#include "Config.h"
#include "pugixml.hpp"
#include "TypeAlias.h"
#include "util/Logger.h"
#include "util/Functions.h"
#include "util/Exceptions.h"
#include "util/NetDefine.h"
#include <iostream>
#include <exception>
#include <cstdio>

namespace symdb {

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
                           const std::string &used_pattern,
                           bool is_from_global)
    : pattern_ { orig_pattern },
      regex_ { std::regex(used_pattern) },
      is_from_global_ { is_from_global }
{
}

RegexPattern::RegexPattern(const std::string &the_pattern, bool is_from_global)
    : RegexPattern(the_pattern, the_pattern, is_from_global)
{
}

ProjectConfig::ProjectConfig(const std::string &name, const std::string &home)
    : name_ { name }
{
    fspath tmp_path { home };
    home_path_ = filesystem::canonical(tmp_path);
}

void ProjectConfig::SetBuildPath(std::string path) {
    LOG_DEBUG << "project=" << name_ << " path=" << path;
    symutil::replace_string(path, "{PROJECT_HOME}", home_path_.string());
    fspath build_path { path };
    filesystem::create_directories(build_path);
    build_path_ = filesystem::canonical(build_path, home_path_);
    LOG_DEBUG << "project=" << name_ << " final_build_path=" << build_path_;
}

void ProjectConfig::AddExcludePattern(const std::string &pattern) {
    std::string used_pattern = pattern;
    symutil::replace_string(used_pattern, "{PROJECT_HOME}", home_path_.string());
    exclude_patterns_.push_back(RegexPattern { pattern, used_pattern });
}

void ProjectConfig::SpecializeGlobalPattern(const std::string &pattern) {
    std::string used_pattern = pattern;
    symutil::replace_string(used_pattern, "{PROJECT_HOME}", home_path_.string());
    if (used_pattern != pattern) {
        exclude_patterns_.push_back(RegexPattern { pattern, used_pattern, true });
    } else {
        LOG_ERROR << "no project info in pattern: " << pattern;
    }
}

bool ProjectConfig::IsFileExcluded(const fspath &path) const {
    for (const auto &rp : exclude_patterns_) {
        if (std::regex_match(path.string(), rp.regex())) {
            return true;
        }
    }

    return ConfigInst.IsFileExcluded(path);
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
    log_path_ = symutil::expand_env(child_value_or_throw(root_node, "LogDir"));
    db_path_ = symutil::expand_env(child_value_or_throw(root_node, "DataDir"));
    listen_path_ = child_value_or_default(root_node, "Listen", symdb::kDefaultSockPath);

    auto ensure_dir_exists = [](const std::string &dir) {
        filesystem::path dir_path(dir);
        filesystem::create_directories(dir_path);
    };

    ensure_dir_exists(db_path_);
    ensure_dir_exists(log_path_);

    filesystem::path log_file = filesystem::path(log_path_) / "symdb.log";

    LoggerInst.Init(symdb::LogLevel::DEBUG, log_file.string());

    InitGlobalExcludePattern(root_node);
    InitProjectsConfig(root_node);
    InitDefaultIncDirs(root_node);
}

void Config::InitGlobalExcludePattern(const pugi::xml_node &root_node)
{
    auto global_exclude_entry = root_node.select_nodes("//GlobalExcluded/ExcludeEntry");
    for (const auto &entry : global_exclude_entry) {
        std::string pattern = entry.node().attribute("pattern").as_string();
        if (pattern.find("{PROJECT_HOME}") != std::string::npos) {
            global_project_patterns_.push_back(pattern);
        } else {
            global_excluded_patterns_.push_back(RegexPattern { pattern });
        }
    }
}

void Config::InitProjectsConfig(const pugi::xml_node &root)
{
    projects_.clear();

    auto project_nodes = root.select_nodes("//Projects/Project");
    projects_.reserve(project_nodes.size());

    for (const auto &proj : project_nodes) {
        const auto &node = proj.node();
        auto name = child_value_or_throw(node, "Name");
        auto home = child_value_or_throw(node, "Home");
        auto expanded_home = symutil::expand_env(home);
        ProjectConfigPtr pc = std::make_shared<ProjectConfig>(name, expanded_home);
        for (const auto &entry : node.children("ExcludeEntry")) {
            std::string cfg_pattern = entry.attribute("pattern").as_string();
            pc->AddExcludePattern(cfg_pattern);
        }

        auto build_dir = child_value_or_default(node, "BuildDir", "_build");
        pc->SetBuildPath(build_dir);

        pc->is_enable_file_watch(node.child("EnableFileWatch").text().as_bool(true));
        for (const auto &pattern : global_project_patterns_) {
            pc->SpecializeGlobalPattern(pattern);
        }
        projects_.push_back(pc);
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
        const std::string command = "g++ -E -x c++ - -v < /dev/null 2>&1";
        const std::string kSysIncSearchBegin = "#include <...> search starts here:";
        const std::string kSysIncSearchEnd = "End of search list.";
        FILE *stream = popen(command.c_str(), "r");
        if (stream == nullptr) {
            THROW_AT_FILE_LINE("popen error: %s", strerror(errno));
        }

        const char *unwanted = " \r\t\n";

        std::vector<std::string> default_inc_dirs;
        bool is_started = false;
        char line[4096];
        while (fgets(line, 4096, stream) != nullptr) {
            char *start = line + strspn(line, unwanted);
            char *end = start + strcspn(start, unwanted);
            std::string str_line { start, end };
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
        pclose(stream);
    }
}

bool Config::IsFileExcluded(const fspath &path) const {
    for (const auto &rp : global_excluded_patterns_) {
        bool is_matched = std::regex_match(path.string(), rp.regex());
        if (is_matched) {
            return true;
        }
    }

    return false;
}

} /* symdb */
