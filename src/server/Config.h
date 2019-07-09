#pragma once

#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>
#include "TypeAlias.h"

namespace pugi {
class xml_node;
}  // namespace pugi

namespace symdb {

class RegexPattern {
public:
  RegexPattern(const std::string &pattern, bool is_from_global = false);

  RegexPattern(const std::string &orig_pattern, const std::string &use_pattern,
               bool is_from_global = false);

  const std::string &pattern() const { return pattern_; }
  const std::regex regex() const { return regex_; }
  bool is_from_global() const { return is_from_global_; }

private:
  std::string pattern_;
  std::regex regex_;
  bool is_from_global_;
};

class ProjectConfig {
public:
  ProjectConfig(const std::string &name, const std::string &home);

  void SetBuildPath(std::string path);

  void AddExcludePattern(const std::string &pattern);
  void SpecializeGlobalPattern(const std::string &pattern);
  bool IsFileExcluded(const fspath &path) const;

  bool is_enable_file_watch() const { return is_enable_file_watch_; }
  const std::string &name() const { return name_; }
  const fspath &home_path() const { return home_path_; }
  const fspath &build_path() const { return build_path_; }

  void is_enable_file_watch(bool is_enabled) {
    is_enable_file_watch_ = is_enabled;
  }

private:
  std::string name_;
  fspath home_path_;
  fspath build_path_;
  std::vector<RegexPattern> exclude_patterns_;
  bool is_enable_file_watch_;
};

using ProjectConfigPtr = std::shared_ptr<ProjectConfig>;

class Config {
public:
  void Init(const std::string &xml_file);

  bool IsFileExcluded(const fspath &path) const;

  const std::string &log_path() const { return log_path_; }
  const std::string &db_path() const { return db_path_; }
  const std::string &listen_path() const { return listen_path_; }
  const StringVec &default_inc_dirs() const { return default_inc_dirs_; }

  const std::vector<ProjectConfigPtr> &projects() { return projects_; };

  static Config &Instance() {
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
  std::vector<ProjectConfigPtr> projects_;
};

}  // namespace symdb

#define ConfigInst symdb::Config::Instance()
