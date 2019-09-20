// Copyright (C) 2011-2018 ycmd contributors
//
// This file is part of ycmd.
//
// ycmd is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// ycmd is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with ycmd.  If not, see <http://www.gnu.org/licenses/>.

#ifndef LOCATION_H_6TLFQH4I
#define LOCATION_H_6TLFQH4I

#include "ClangUtils.h"

#include <clang-c/Index.h>
#include <iostream>
#include <string>
#include "proto/General.pb.h"

namespace symdb {

class Location {
public:
  // Creates an invalid location
  Location() : line_number_(0), column_number_(0), filename_("") {}

  Location(const std::string &filename, unsigned int line, unsigned int column)
      : line_number_(line), column_number_(column), filename_(filename) {}

  Location(const CXSourceLocation &location) {
    CXFile file;
    unsigned int unused_offset;
    clang_getExpansionLocation(location, &file, &line_number_, &column_number_,
                               &unused_offset);
    filename_ = CXFileToFilepath(file);
  }

  Location(const CXCursor &cursor)
      : Location(clang_getCursorLocation(cursor)) {}

  explicit Location(const PB_Location &pb_loc) {
    filename_ = pb_loc.path();
    line_number_ = pb_loc.line();
    column_number_ = pb_loc.column();
  }

  bool operator==(const Location &other) const {
    return line_number_ == other.line_number_ &&
           column_number_ == other.column_number_ &&
           filename_ == other.filename_;
  }

  const std::string &filename() const { return filename_; }
  uint32_t line_number() const { return line_number_; }
  uint32_t column_number() const { return column_number_; }

  bool IsValid() const { return !filename_.empty(); }

  void Serialize(PB_Location &pb_loc) const {
    pb_loc.set_path(filename_);
    pb_loc.set_column(column_number_);
    pb_loc.set_line(line_number_);
  }

private:
  uint32_t line_number_;
  uint32_t column_number_;

  // The full, absolute path
  std::string filename_;
};

inline std::ostream &operator<<(std::ostream &os, const Location &loc) {
  return os << loc.filename() << "["
            << "line=" << loc.line_number() << ",col=" << loc.column_number()
            << "]";
}

}  // namespace symdb

#endif /* end of include guard: LOCATION_H_6TLFQH4I */
