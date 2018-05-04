//===--- LevelDBIndex.h - Index for files. ------------------------- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// LevelDBIndex
//
//===---------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_LEVELDBINDEX_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_LEVELDBINDEX_H

#include "../ClangdUnit.h"
#include "Index.h"

#include "multiple-store.h"

namespace clang {
namespace clangd {

/// \brief This manages symbols on all symbols.
class LevelDBIndex : public SymbolIndex {
public:
  bool open(PathRef DBPath);

  ~LevelDBIndex();

  /// \brief Update symbols in \p Path with symbols in \p AST. If \p AST is
  /// nullptr, this removes all symbols in the file
  void update(PathRef Path, ParsedAST *AST);

  bool
  fuzzyFind(const FuzzyFindRequest &Req,
            llvm::function_ref<void(const Symbol &)> Callback) const override;

  void lookup(const LookupRequest &Req,
              llvm::function_ref<void(const Symbol &)> Callback) const override;

private:
  std::unique_ptr<multiplestore::MultiDB> IndexDB;
  std::unique_ptr<multiplestore::Comparator> Comparator;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_LEVELDBINDEX_H
