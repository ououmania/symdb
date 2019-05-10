#include "TranslationUnit.h"
#include "ClangUtils.h"
#include "util/Logger.h"
#include <iostream>

namespace symdb
{

TranslationUnit::TranslationUnit(
  const std::string &filename,
  const std::vector< std::string > &flags,
  CXIndex clang_index)
{
    std::vector< const char * > pointer_flags;
    pointer_flags.reserve( flags.size() );

    for ( const std::string & flag : flags ) {
        pointer_flags.push_back( flag.c_str() );
    }

    // Actually parse the translation unit.
    CXErrorCode failure = clang_parseTranslationUnit2(
                            clang_index,
                            filename.c_str(),
                            &pointer_flags[ 0 ],
                            pointer_flags.size(),
                            nullptr,
                            0,
                            0,
                            &translation_unit_ );
    if ( failure != CXError_Success ) {
        throw symdb::ClangParseError( failure );
    }

    filename_ = filename;
}

TranslationUnit::~TranslationUnit()
{
    if (translation_unit_ != nullptr)
    {
        clang_disposeTranslationUnit(translation_unit_);
    }
}

void TranslationUnit::CollectSymbols()
{
    CXCursor cursor = clang_getTranslationUnitCursor(translation_unit_);

    (void) clang_visitChildren(cursor, &TranslationUnit::VisitCursor, this);
}

CXChildVisitResult TranslationUnit::VisitCursor(
    CXCursor cursor,
    CXCursor parent,
    CXClientData client_data)
{
    (void) parent;

    CXSourceLocation cursor_location = clang_getCursorLocation(cursor);
    Location location(cursor_location);

    TranslationUnit *unit = reinterpret_cast<TranslationUnit*>(client_data);
    if (location.filename() == unit->filename_ && clang_isCursorDefinition(cursor)) {
        auto symbol = symdb::CXStringToString(clang_getCursorSpelling(cursor));
        if (!symbol.empty() && IsWantedCursor(cursor)) {
            CXType cursorType = clang_getCursorType(cursor);
            auto typeStr = symdb::CXStringToString(clang_getTypeSpelling(cursorType));
            auto cursorKind = clang_getCursorKind(cursor);
            auto kindStr = symdb::CXStringToString(clang_getCursorKindSpelling(cursorKind));

            // LOG_DEBUG << "symbol=" << symbol << ", type=" << typeStr << ", kind="
            //           << cursorKind << ":" <<  kindStr << ", " << location;

            auto usr = symdb::CXStringToString(clang_getCursorUSR(cursor));

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

Location TranslationUnit::GetSourceLocation(
    const std::string &filename,
    unsigned int line,
    unsigned int column) const
{
    CXCursor cursor = GetReferencedCursor(filename, line, column);

    CXString cx_symbol = clang_getCursorUSR(cursor);
    auto symbol = symdb::CXStringToString(cx_symbol);
    if (symbol.empty()) {
        return Location();
    }

    return Location(clang_getCursorLocation( cursor ));
}

std::string TranslationUnit::GetReferencedSymbol(
    const std::string &file,
    unsigned int line,
    unsigned int column) const
{
    CXCursor cursor = GetReferencedCursor(file, line, column);
    CXString cx_symbol = clang_getCursorUSR(cursor);
    auto symbol = symdb::CXStringToString(cx_symbol);

    return symbol;
}

CXCursor TranslationUnit::GetReferencedCursor(
    const std::string &filename,
    unsigned int line,
    unsigned int column) const
{
    CXFile file = clang_getFile(translation_unit_, filename.c_str());
    auto location = clang_getLocation(translation_unit_, file, line, column);
    auto cursor = clang_getCursor(translation_unit_, location);
    CXCursor referenced_cursor = clang_getCursorReferenced(cursor);
    auto canonical_cursor = clang_getCanonicalCursor( referenced_cursor );

    if ( !symdb::CursorIsValid( canonical_cursor ) ) {
        return referenced_cursor;
    }

    return canonical_cursor;
}

// We only consider non-static definitions.
bool TranslationUnit::IsWantedCursor(CXCursor cursor)
{
    auto kind = clang_getCursorKind(cursor);
    switch (kind) {
    case CXCursorKind::CXCursor_CXXMethod:
    case CXCursorKind::CXCursor_Constructor:
    case CXCursorKind::CXCursor_Destructor:
        return true;

    case CXCursorKind::CXCursor_StructDecl:
    case CXCursorKind::CXCursor_TypedefDecl:
    case CXCursorKind::CXCursor_TypeAliasDecl:
    case CXCursorKind::CXCursor_FunctionTemplate:
    case CXCursorKind::CXCursor_FunctionDecl:
    case CXCursorKind::CXCursor_VarDecl:
        return clang_Cursor_getStorageClass(cursor) != CX_StorageClass::CX_SC_Static;

    default:
        return false;
    }

}

} /*  symdb */
