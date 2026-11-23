//===--- Path.cpp -------------------------------------------*- C++-*------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "support/Path.h"
#include "llvm/Support/Path.h"
#include <cctype>
namespace clang {
namespace clangd {

#ifdef CLANGD_PATH_CASE_INSENSITIVE
std::string maybeCaseFoldPath(PathRef Path) { return Path.lower(); }
bool pathEqual(PathRef A, PathRef B) { return A.equals_insensitive(B); }
#else  // NOT CLANGD_PATH_CASE_INSENSITIVE
std::string maybeCaseFoldPath(PathRef Path) { return Path.str(); }
bool pathEqual(PathRef A, PathRef B) { return A == B; }
#endif // CLANGD_PATH_CASE_INSENSITIVE

PathRef absoluteParent(PathRef Path) {
  assert(llvm::sys::path::is_absolute(Path));
#if defined(_WIN32)
  // llvm::sys says "C:\" is absolute, and its parent is "C:" which is relative.
  // This unhelpful behavior seems to have been inherited from boost.
  if (llvm::sys::path::relative_path(Path).empty()) {
    return PathRef();
  }
#endif
  PathRef Result = llvm::sys::path::parent_path(Path);
  assert(Result.empty() || llvm::sys::path::is_absolute(Result));
  return Result;
}

bool pathStartsWith(PathRef Ancestor, PathRef Path,
                    llvm::sys::path::Style Style) {
  assert(llvm::sys::path::is_absolute(Ancestor) &&
         llvm::sys::path::is_absolute(Path));
  // If ancestor ends with a separator drop that, so that we can match /foo/ as
  // a parent of /foo.
  if (llvm::sys::path::is_separator(Ancestor.back(), Style))
    Ancestor = Ancestor.drop_back();
  // Ensure Path starts with Ancestor.
  if (!pathEqual(Ancestor, Path.take_front(Ancestor.size())))
    return false;
  Path = Path.drop_front(Ancestor.size());
  // Then make sure either two paths are equal or Path has a separator
  // afterwards.
  return Path.empty() || llvm::sys::path::is_separator(Path.front(), Style);
}

std::string normalizePathDriveLetter(PathRef Path, bool UpperCase) {
  std::string Result = Path.str();
#if defined(_WIN32)
  if (Result.size() >= 2 && Result[1] == ':' &&
      ((Result[0] >= 'a' && Result[0] <= 'z') ||
       (Result[0] >= 'A' && Result[0] <= 'Z'))) {
    Result[0] = UpperCase ? static_cast<char>(std::toupper(
                                static_cast<unsigned char>(Result[0])))
                          : static_cast<char>(std::tolower(
                                static_cast<unsigned char>(Result[0])));
  }
#else
  (void)UpperCase;
#endif
  return Result;
}
} // namespace clangd
} // namespace clang
