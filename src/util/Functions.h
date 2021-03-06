#pragma once

#include <algorithm>
#include <boost/filesystem.hpp>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#if __GNU_C__ >= 5
#include <regex>
#else
#include <boost/regex.hpp>
#endif

namespace symutil {

struct FunctionRunnerGuard {
  template <typename FuncType>
  FunctionRunnerGuard(FuncType func) : func_{func} {}

  ~FunctionRunnerGuard() { func_(); }

private:
  std::function<void()> func_;
};

inline const std::string &to_string(const std::string &s) { return s; }
inline std::string to_string(const char *s) { return std::string(s); }
inline std::string to_string(const boost::filesystem::path &path) {
  return path.string();
}

template <typename Arg1, typename... Args>
inline std::string str_join(const char *delim, Arg1 &&arg1) {
  using std::to_string;
  using symutil::to_string;

  (void)delim;

  return to_string(arg1);
}

template <typename Arg1, typename... Args>
inline std::string str_join(const char *delim, Arg1 &&arg1, Args &&... args) {
  using std::to_string;
  using symutil::to_string;

  return to_string(arg1) + delim + str_join(delim, std::forward<Args>(args)...);
}

inline bool path_has_prefix(const boost::filesystem::path &path,
                            const boost::filesystem::path &prefix) {
  const std::string &path_str = path.string();
  const std::string &prefix_str = prefix.string();

  if (prefix_str.size() > path_str.size()) {
    return false;
  }

  return path_str.compare(0, prefix_str.size(), prefix_str) == 0;
}

inline void replace_string(std::string &dest, const std::string &from,
                           const std::string &to) {
  auto pos = dest.find(from);
  if (pos != std::string::npos) {
    dest.replace(pos, from.size(), to);
  }
}

// wordexp requires glibc and expands too much.
#if __GNU_C__ >= 5
inline std::string expand_env(std::string text) {
  static const std::regex env_re{R"--(\$\{([^}]+)\})--"};
  std::smatch match;
  while (std::regex_search(text, match, env_re)) {
    auto const from = match[0];
    auto const var_name = match[1].str().c_str();
    text.replace(from.first, from.second, std::getenv(var_name));
  }
  return text;
}
#else
inline std::string matchToEnv(const boost::smatch& match) {
  const char *s = getenv(match[1].str().c_str());
  return s == NULL ? std::string("") : std::string(s);
}

inline std::string expand_env(std::string text) {
  static const boost::regex env_re { "\\$\\{([^}]+)\\}" };
  return boost::regex_replace(text, env_re, matchToEnv);
}
#endif

}  // namespace symutil
