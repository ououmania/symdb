# pragma once

#include <string>
#include <algorithm>
#include <boost/filesystem.hpp>

namespace symutil {

inline const std::string& to_string(const std::string &s) { return s; }
inline std::string to_string(const char *s) { return std::string(s); }
inline std::string to_string(const boost::filesystem::path &path) { return path.string(); }

template <typename Arg1, typename... Args>
inline std::string
str_join(const char *delim, Arg1 &&arg1) {
    using std::to_string;
    using symutil::to_string;

    (void) delim;

    return to_string(arg1);
}

template <typename Arg1, typename... Args>
inline std::string
str_join(const char *delim, Arg1 &&arg1, Args&&... args) {
    using std::to_string;
    using symutil::to_string;

    return to_string(arg1) + delim + str_join(delim, std::forward<Args>(args)...);
}

inline bool
path_has_prefix(const boost::filesystem::path & path, const boost::filesystem::path & prefix)
{
    auto pair = std::mismatch(path.begin(), path.end(), prefix.begin(), prefix.end());
    return pair.second == prefix.end();
}

inline void
replace_string(std::string &dest, const std::string &from, const std::string &to) {
    auto pos = dest.find(from);
    if (pos != std::string::npos) {
        dest.replace(pos, from.size(), to);
    }
}

} /* symutil  */
