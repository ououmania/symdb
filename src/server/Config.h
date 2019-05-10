# pragma once

#include "TypeAlias.h"
#include <string>
#include <set>
#include <map>
#include <vector>
#include <regex>

namespace pugi {
    class xml_node;
} /* pugi  */

namespace symdb {

struct RegexPattern {
    explicit RegexPattern(const std::string &the_pattern);
    RegexPattern(const std::string &orig_pattern,
                 const std::string &use_pattern);
    std::string pattern;
    std::regex  reg;
    bool is_copied_from_global;
};

struct ProjectConfig {
    std::string name;
    std::string home_path;
    std::vector<RegexPattern> exclude_patterns;
};

using ProjectConfigPtr = std::shared_ptr<ProjectConfig>;

class Config {
public:
    void Init(const std::string &xml_file);

    const std::string& log_path() const { return log_path_; }
    const std::string& db_path() const { return db_path_; }
    const std::string& listen_path() const { return listen_path_; }
    const StringVec& default_inc_dirs() const { return default_inc_dirs_; }

    bool IsFileExcluded(const fspath &path) const;

    static Config& Instance()
    {
        static Config obj;
        return obj;
    }

private:
    void InitDefaultIncDirs(const pugi::xml_node &root);

    void InitGlobalExcludePattern(const pugi::xml_node &root);

    void InitProjectsConfig(const pugi::xml_node &root);

private:
    Config() = default;

    std::string db_path_;
    std::string log_path_;
    std::string listen_path_;
    StringVec default_inc_dirs_;
    std::vector<RegexPattern> global_excluded_patterns_;
    std::vector<std::string> global_project_patterns_;
    std::vector<ProjectConfigPtr> project_config_;
};

} // namespace symdb

#define ConfigInst symdb::Config::Instance()

