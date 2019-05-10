# pragma once

#include "TypeAlias.h"
#include <string>
#include <set>
#include <map>
#include <vector>
#include <regex>

namespace symdb {

class Config {
public:
    void Init(const std::string &xml_file);

    const std::string& log_path() const { return log_path_; }
    const std::string& db_path() const { return db_path_; }
    const StringVec& default_inc_dirs() const { return default_inc_dirs_; }

    bool IsFileExcluded(const fspath &path) const;

    static Config& Instance()
    {
        static Config obj;
        return obj;
    }

private:
    void InitDefaultIncDirs();

private:
    Config() = default;

    std::string db_path_;
    std::string log_path_;
    StringVec default_inc_dirs_;
    std::vector<std::regex> excluded_file_patterns_;
};

} // namespace symdb

#define ConfigInst symdb::Config::Instance()

