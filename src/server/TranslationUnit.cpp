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
      nullptr, 0, CXTranslationUnit_DetailedPreprocessingRecord,
      &translation_unit_);
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

  do {
    if (location.filename() != unit->filename_) {
      break;
    }

    auto symbol = CXStringToString(clang_getCursorSpelling(cursor));
    if (symbol.empty()) {
      break;
    }

    CXType cursorType = clang_getCursorType(cursor);
    CXCursorKind cursorKind = clang_getCursorKind(cursor);

    LineColPair lcp(location.line_number(), location.column_number());
    if (cursorKind == CXCursor_MacroExpansion) {
      unit->macro_expansions_.insert(lcp);
      break;
    }

    // FIXME: Some tokens are lost here. All the statements of the expansion
    // have the same location.
    if (unit->macro_expansions_.find(lcp) != unit->macro_expansions_.end()) {
      break;
    }

    bool is_definition = clang_isCursorDefinition(cursor);
    if (is_definition && IsWantedDefinition(cursor)) {
      auto usr = CXStringToString(clang_getCursorUSR(cursor));
      if (usr.empty()) {
        LOG_ERROR << "No USR name at " << location;
      } else {
        unit->defined_symbols_[usr] = location;
      }
    } else if (!is_definition && IsWantedReference(cursor)) {
      auto referencedCursor = clang_getCursorReferenced(cursor);

      if (IsWantedReferenceDef(referencedCursor)) {
        auto usr = CXStringToString(clang_getCursorUSR(referencedCursor));
        Location origin_loc { referencedCursor };
        LOG_DEBUG << "Refer " << usr << " of file " << origin_loc.filename()
                  << " at " << location;

        SymbolPathPair symbol_path { usr, origin_loc.filename() };
        unit->referred_symbols_[symbol_path].insert(
            LineColPair{location.line_number(), location.column_number()});
      } else {
        CXCursorKind kind = clang_getCursorKind(referencedCursor);
        auto usr = CXStringToString(clang_getCursorUSR(referencedCursor));
        Location origin_loc { referencedCursor };
        LOG_DEBUG << "Exclude " << usr << " of file " << origin_loc.filename()
                  << " at " << location << ", kind=" << kind;
      }
    }
  } while (false);

  CXCursorKind kind = clang_getCursorKind(cursor);
  if (kind >= CXCursor_FirstExpr && kind <= CXCursor_LastStmt) {
    return CXChildVisit_Recurse;
  }

  switch (kind) {
    case CXCursor_Namespace:
    case CXCursor_ClassDecl:
    case CXCursor_StructDecl:
    case CXCursor_FunctionDecl:
    case CXCursor_VarDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_CallExpr:
      return CXChildVisit_Recurse;

    default:
      break;
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

bool TranslationUnit::IsWantedDefinition(CXCursor cursor) {
  CXCursorKind kind = clang_getCursorKind(cursor);
  switch (kind) {
    // We don't ignore private methods.
    case CXCursorKind::CXCursor_CXXMethod:
    case CXCursorKind::CXCursor_Constructor:
      return true;

    case CXCursorKind::CXCursor_StructDecl:
    case CXCursorKind::CXCursor_ClassDecl:
    case CXCursorKind::CXCursor_TypedefDecl:
    case CXCursorKind::CXCursor_TypeAliasDecl:
    case CXCursorKind::CXCursor_FunctionTemplate:
    case CXCursorKind::CXCursor_ClassTemplate:
    case CXCursorKind::CXCursor_FunctionDecl:
    case CXCursorKind::CXCursor_VarDecl: {
      // Haven't found a proper way to know if the variable/function is a
      // local one. clang_Cursor_getStorageClass() fails to do this since
      // it returns CX_SC_None if the storage class is not specified.
      CXLinkageKind linkageKind = clang_getCursorLinkage(cursor);
      return linkageKind == CXLinkage_UniqueExternal ||
             linkageKind == CXLinkage_External;
    }

    default:
      return false;
  }
}

bool TranslationUnit::IsWantedReference(CXCursor cursor) {
  std::string spelling = CXStringToString(clang_getCursorSpelling(cursor));
  if (spelling.find("operator", 0) != std::string::npos) {
    return false;
  }

  CXCursorKind kind = clang_getCursorKind(cursor);
  switch (kind) {
    case CXCursorKind::CXCursor_TypeRef:
    case CXCursorKind::CXCursor_MemberRef:
    case CXCursorKind::CXCursor_MemberRefExpr:
    case CXCursorKind::CXCursor_TemplateRef:
      return true;

    case CXCursorKind::CXCursor_DeclRefExpr: {
      CXCursor referencedCursor = clang_getCursorReferenced(cursor);
      CXLinkageKind linkageKind = clang_getCursorLinkage(referencedCursor);
      return linkageKind == CXLinkage_UniqueExternal ||
             linkageKind == CXLinkage_External;
    }

    default:
      return false;
  }
}

// We only consider non-static definitions.
bool TranslationUnit::IsWantedReferenceDef(CXCursor cursor) {
  std::string ns = symdb::GetCursorNamespace(cursor);
  if (ns == "std" || ns == "boost") {
    return true;
  }

  CXCursorKind kind = clang_getCursorKind(cursor);
  switch (kind) {
    // We don't ignore private methods.
    case CXCursorKind::CXCursor_CXXMethod:
    case CXCursorKind::CXCursor_Constructor:
    case CXCursorKind::CXCursor_FunctionDecl:
      return true;

    case CXCursorKind::CXCursor_EnumConstantDecl:
    case CXCursorKind::CXCursor_VarDecl:
    case CXCursorKind::CXCursor_StructDecl:
    case CXCursorKind::CXCursor_ClassDecl:
    case CXCursorKind::CXCursor_TypedefDecl:
    case CXCursorKind::CXCursor_TypeAliasDecl:
    case CXCursorKind::CXCursor_FunctionTemplate:
    case CXCursorKind::CXCursor_ClassTemplate: {
      // Haven't found a proper way to know if the variable/function is a
      // local one. clang_Cursor_getStorageClass() fails to do this since
      // it returns CX_SC_None if the storage class is not specified.
      CXLinkageKind linkageKind = clang_getCursorLinkage(cursor);
      return linkageKind == CXLinkage_UniqueExternal ||
             linkageKind == CXLinkage_External;
    }

    default:
      return false;
  }
}

}  // namespace symdb
