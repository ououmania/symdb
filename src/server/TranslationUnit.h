#ifndef TRANSLATIONUNIT_H_I2JH68ZR
#define TRANSLATIONUNIT_H_I2JH68ZR

#include <clang-c/Index.h>
#include <map>
#include <string>
#include <vector>
#include "Location.h"

namespace symdb {

using LineColPair = std::pair<uint32_t, uint32_t>;
using LineColPairSet = std::set<LineColPair>;
using SymbolPathPair = std::pair<std::string, std::string>;
using SymbolDefinitionMap = std::map<std::string, Location>;
using SymbolReferenceMap = std::map<SymbolPathPair, LineColPairSet>;

class TranslationUnit {
public:
  TranslationUnit(const std::string &filename,
                  const std::vector<std::string> &flags, CXIndex clang_index);

  ~TranslationUnit();

  // Not thread-safe
  void CollectSymbols();

  CXCursor GetReferencedCursor(const std::string &file, unsigned int line,
                               unsigned int column) const;

  std::string GetReferencedSymbol(const std::string &file, unsigned int line,
                                  unsigned int column) const;

  Location GetSourceLocation(const std::string &file, unsigned int line,
                             unsigned int column) const;

  Location GetSourceLocation(unsigned int line, unsigned int column) const {
    return GetSourceLocation(filename_, line, column);
  }

  SymbolDefinitionMap& defined_symbols() { return defined_symbols_; }
  SymbolReferenceMap& reference_symbols() { return referred_symbols_; }

private:
  void CheckClangDiagnostic();

private:
  static CXChildVisitResult VisitCursor(CXCursor cursor, CXCursor parent,
                                        CXClientData client_data);

  static bool IsWantedDefinition(CXCursor cursor);
  static bool IsWantedReference(CXCursor cursor);
  // The definition of the reference
  static bool IsWantedReferenceDef(CXCursor cursor);

  CXTranslationUnit translation_unit_;
  std::string filename_;
  SymbolDefinitionMap defined_symbols_;
  SymbolReferenceMap referred_symbols_;
  std::set<LineColPair> macro_expansions_;
};

}  // namespace symdb

#endif /* end of include guard: TRANSLATIONUNIT_H_I2JH68ZR */
