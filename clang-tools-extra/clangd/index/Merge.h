//===--- Merge.h -------------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_MERGE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_MERGE_H

#include "index/Index.h"

namespace clang {
namespace clangd {

// Merge symbols L and R, preferring data from L in case of conflict.
// The two symbols must have the same ID.
// Returned symbol may contain data owned by either source.
Symbol mergeSymbol(const Symbol &L, const Symbol &R);

// MergedIndex is a composite index based on two provided Indexes:
//  - the Dynamic index covers few files, but is relatively up-to-date.
//  - the Static index covers a bigger set of files, but is relatively stale.
// The returned index attempts to combine results, and avoid duplicates.
class MergedIndex : public SymbolIndex {
  const SymbolIndex *Dynamic, *Static;

public:
  // The constructor does not access the symbols.
  // It's safe to inherit from this class and pass pointers to derived members.
  MergedIndex(const SymbolIndex *Dynamic, const SymbolIndex *Static)
      : Dynamic(Dynamic), Static(Static) {}

  bool fuzzyFind(const FuzzyFindRequest &,
                 llvm::function_ref<void(const Symbol &)>) const override;
  void lookup(const LookupRequest &,
              llvm::function_ref<void(const Symbol &)>) const override;
  bool refs(const RefsRequest &,
            llvm::function_ref<void(const Ref &)>) const override;
  bool containedRefs(
      const ContainedRefsRequest &,
      llvm::function_ref<void(const ContainedRefsResult &)>) const override;
  void relations(const RelationsRequest &,
                 llvm::function_ref<void(const SymbolID &, const Symbol &)>)
      const override;
  void
  reverseRelations(const RelationsRequest &,
                   llvm::function_ref<void(const SymbolID &, const Symbol &)>)
      const override;
  llvm::unique_function<IndexContents(llvm::StringRef) const>
  indexedFiles() const override;
  size_t estimateMemoryUsage() const override {
    return Dynamic->estimateMemoryUsage() + Static->estimateMemoryUsage();
  }

  size_t collectAllSymbols(SymbolSimplifiedMap &Map) const override;
  uint64_t generation() const override {
    uint64_t DG = Dynamic->generation();
    uint64_t SG = Static->generation();
    if (DG == 0 && SG == 0)
      return 0;
    return DG ^ (SG * 0x9e3779b97f4a7c15ULL + 0x9e3779b97f4a7c15ULL +
                 (DG << 6) + (DG >> 2));
  }
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_MERGE_H
