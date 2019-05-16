#ifndef TRANSLATIONUNIT_H_I2JH68ZR
#define TRANSLATIONUNIT_H_I2JH68ZR

#include <vector>
#include <map>
#include <string>
#include <clang-c/Index.h>
#include "Location.h"

namespace symdb
{

using SymbolMap = std::map<std::string, Location>;

class TranslationUnit
{
public:
    TranslationUnit(
        const std::string &filename,
        const std::vector< std::string > &flags,
        CXIndex clang_index);

    ~TranslationUnit();

    // Not thread-safe
    void CollectSymbols();

    CXCursor GetReferencedCursor(
        const std::string &file,
        unsigned int line,
        unsigned int column) const;

    std::string GetReferencedSymbol(
        const std::string &file,
        unsigned int line,
        unsigned int column) const;

    Location GetSourceLocation(
        const std::string &file,
        unsigned int line,
        unsigned int column) const;

    Location GetSourceLocation(unsigned int line, unsigned int column) const {
        return GetSourceLocation(filename_, line, column);
    }

    SymbolMap& defined_symbols() { return defined_symbols_; }

private:
    void CheckClangDiagnostic();

private:
    static CXChildVisitResult VisitCursor(
        CXCursor cursor,
        CXCursor parent,
        CXClientData client_data);

    static bool IsWantedCursor(CXCursor cursor);

    CXTranslationUnit translation_unit_;
    std::string filename_;
    SymbolMap defined_symbols_;
};

} // namespace ns

#endif /* end of include guard: TRANSLATIONUNIT_H_I2JH68ZR */
