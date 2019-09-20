// Copyright (C) 2011, 2012 Google Inc.
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

#ifndef CLANGUTILS_H_9MVHQLJS
#define CLANGUTILS_H_9MVHQLJS

#include <clang-c/Index.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace symdb {

/**
 * Return a std::string from the supplied CXString.
 *
 * Takes ownership of, and destroys, the supplied CXString which must not be
 * used subsequently.
 */
std::string CXStringToString(CXString text);

bool CursorIsValid(CXCursor cursor);

std::string CXFileToFilepath(CXFile file);

std::string ClangVersion();

const char* CXErrorCodeToString(CXErrorCode code);

/**
 * Thrown when libclang fails to parse (or reparse) the translation unit.
 */
struct ClangParseError : std::runtime_error {
  ClangParseError(const char* what_arg);
  ClangParseError(CXErrorCode code);
};

inline std::ostream& operator<<(std::ostream& stream, const CXString& str) {
  stream << clang_getCString(str);
  clang_disposeString(str);
  return stream;
}

inline std::string GetCursorNamespace(CXCursor cursor) {
  std::string ns;
  while (!clang_Cursor_isNull(cursor)) {
    CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_TranslationUnit) {
      break;
    }
    if (kind == CXCursor_Namespace) {
      ns = CXStringToString(clang_getCursorSpelling(cursor));
    }
    cursor = clang_getCursorSemanticParent(cursor);
  }
  return ns;
}

}  // namespace symdb

#endif /* end of include guard: CLANGUTILS_H_9MVHQLJS */
