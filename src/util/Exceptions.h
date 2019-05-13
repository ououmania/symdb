# pragma once

#include <filesystem>
#include <cstdarg>
#include <boost/filesystem.hpp>
#include <cstdio>

namespace symdb {

class GeneralException : public std::exception {
public:
    GeneralException(const char *fmt, ...) {
        va_list arg_list;
        va_start(arg_list, fmt);
        vsnprintf(buffer_, sizeof(buffer_), fmt, arg_list);
        va_end(arg_list);
    }
    const char *what() const throw() { return buffer_; }

private:
    char buffer_[512];
};

} // namespace symdb

#define THROW_AT_FILE_LINE(__fmt, ...)                                         \
    do {                                                                       \
        boost::filesystem::path path(__FILE__);                                \
        throw symdb::GeneralException("%s:%d " __fmt, path.filename().c_str(), \
                __LINE__, ##__VA_ARGS__);                                      \
    } while (false)

