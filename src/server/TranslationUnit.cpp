#include "TranslationUnit.h"
#include <iostream>
#include "ClangUtils.h"
#include "TypeAlias.h"
#include "util/Logger.h"

namespace symdb {

TranslationUnit::TranslationUnit(const std::string &filename,
                                 const std::vector<std::string> &flags,
                                 CXIndex clang_index) {
  std::vector<const char *> pointer_flags;
  pointer_flags.reserve(flags.size());

  for (const std::string &flag : flags) {
    pointer_flags.push_back(flag.c_str());
  }

  // Actually parse the translation unit.
  CXErrorCode failure = clang_parseTranslationUnit2(
      clang_index, filename.c_str(), &pointer_flags[0], pointer_flags.size(),
      nullptr, 0, CXTranslationUnit_None, &translation_unit_);
  if (failure != CXError_Success) {
    throw ClangParseError(failure);
  }

  filename_ = filename;

  CheckClangDiagnostic();
}

TranslationUnit::~TranslationUnit() {
  clang_disposeTranslationUnit(translation_unit_);
}

void TranslationUnit::CheckClangDiagnostic() {
  unsigned num = clang_getNumDiagnostics(translation_unit_);
  if (num == 0) {
    return;
  }

  LOG_ERROR << "file=" << filename_ << " nr_diag=" << num;
  for (unsigned i = 0; i < std::min(num, 3U); i++) {
    CXDiagnostic diag = clang_getDiagnostic(translation_unit_, i);
    if (!diag) {
      continue;
    }

    std::string text = CXStringToString(clang_getDiagnosticSpelling(diag));
    Location location{clang_getDiagnosticLocation(diag)};
    clang_disposeDiagnostic(diag);

    LOG_ERROR << "diagnostic " << i + 1 << ": " << text << location;
  }
}

void TranslationUnit::CollectSymbols() {
  CXCursor cursor = clang_getTranslationUnitCursor(translation_unit_);

  (void)clang_visitChildren(cursor, &TranslationUnit::VisitCursor, this);
}

CXChildVisitResult TranslationUnit::VisitCursor(CXCursor cursor,
                                                CXCursor parent,
                                                CXClientData client_data) {
  (void)parent;

  Location location(clang_getCursorLocation(cursor));

  TranslationUnit *unit = reinterpret_cast<TranslationUnit *>(client_data);
  if (location.filename() == unit->filename_ &&
      clang_isCursorDefinition(cursor)) {
    auto symbol = CXStringToString(clang_getCursorSpelling(cursor));
    if (!symbol.empty() && IsWantedCursor(cursor)) {
      auto usr = CXStringToString(clang_getCursorUSR(cursor));
      if (usr.empty()) {
        LOG_ERROR << "No USR name at " << location;
      } else {
        unit->defined_symbols_[usr] = location;
      }
    }
  }

  CXCursorKind kind = clang_getCursorKind(cursor);
  if (kind == CXCursor_Namespace || kind == CXCursor_ClassDecl) {
    return CXChildVisit_Recurse;
  }

  return CXChildVisit_Continue;
}

Location TranslationUnit::GetSourceLocation(const std::string &filename,
                                            unsigned int line,
                                            unsigned int column) const {
  CXCursor cursor = GetReferencedCursor(filename, line, column);

  CXString cx_symbol = clang_getCursorUSR(cursor);
  auto symbol = CXStringToString(cx_symbol);
  if (symbol.empty()) {
    return Location();
  }

  return Location(clang_getCursorLocation(cursor));
}

std::string TranslationUnit::GetReferencedSymbol(const std::string &file,
                                                 unsigned int line,
                                                 unsigned int column) const {
  CXCursor cursor = GetReferencedCursor(file, line, column);
  CXString cx_symbol = clang_getCursorUSR(cursor);
  auto symbol = CXStringToString(cx_symbol);

  return symbol;
}

CXCursor TranslationUnit::GetReferencedCursor(const std::string &filename,
                                              unsigned int line,
                                              unsigned int column) const {
  CXFile file = clang_getFile(translation_unit_, filename.c_str());
  auto location = clang_getLocation(translation_unit_, file, line, column);
  auto cursor = clang_getCursor(translation_unit_, location);
  CXCursor referenced_cursor = clang_getCursorReferenced(cursor);
  auto canonical_cursor = clang_getCanonicalCursor(referenced_cursor);

  if (!CursorIsValid(canonical_cursor)) {
    return referenced_cursor;
  }

  return canonical_cursor;
}

// We only consider non-static definitions.
bool TranslationUnit::IsWantedCursor(CXCursor cursor) {
  auto kind = clang_getCursorKind(cursor);
  switch (kind) {
    // We don't ignore private methods.
    case CXCursorKind::CXCursor_CXXMethod:
    case CXCursorKind::CXCursor_Constructor:
      return true;

    case CXCursorKind::CXCursor_StructDecl:
    case CXCursorKind::CXCursor_TypedefDecl:
    case CXCursorKind::CXCursor_TypeAliasDecl:
    case CXCursorKind::CXCursor_FunctionDecl:
    case CXCursorKind::CXCursor_VarDecl:
      return clang_Cursor_getStorageClass(cursor) !=
             CX_StorageClass::CX_SC_Static;

    default:
      return false;
  }
}

}  // namespace symdb
