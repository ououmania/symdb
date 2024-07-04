#pragma once

#include <cstdarg>
#include <cstdio>
#include "TypeAlias.h"

namespace symutil {

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

}  // namespace symutil

#define THROW_AT_FILE_LINE(__fmt, ...)                                       \
  do {                                                                       \
    fspath path(__FILE__);                                                   \
    throw symutil::GeneralException("%s:%d " __fmt, path.filename().c_str(), \
                                    __LINE__, ##__VA_ARGS__);                \
  } while (false)
