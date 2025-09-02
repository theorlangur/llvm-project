#include "AST.h"
#include "FindTarget.h"
#include "HeaderSourceSwitch.h"
#include "ParsedAST.h"
#include "Selection.h"
#include "SourceCode.h"
#include "index/Index.h"
#include "index/SymbolLocation.h"
#include "refactor/Tweak.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include <cstddef>
#include <optional>
#include <string>

namespace clang {
namespace clangd {
namespace {
// Deduces the FunctionDecl from a selection. Requires either the function body
// or the function decl to be selected. Returns null if none of the above
// criteria is met.
// FIXME: This is shared with define inline, move them to a common header once
// we have a place for such.
const CXXMethodDecl *getSelectedMethod(const SelectionTree::Node *SelNode) {
  if (!SelNode)
    return nullptr;
  const DynTypedNode &AstNode = SelNode->ASTNode;
  if (const CXXMethodDecl *FD = AstNode.get<CXXMethodDecl>())
    return FD;
  return nullptr;
}

/// Moves definition of a function/method to an appropriate implementation file.
///
/// Before:
/// a.h
///   void foo();
/// a.cc
///   #include "a.h"
///
/// ----------------
///
/// After:
/// a.h
///   void foo();
/// a.cc
///   #include "a.h"
///   void foo() { return; }
class AddRemoveMethodConst : public Tweak {
public:
  const char *id() const override;

  bool hidden() const override { return false; }
  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }
  std::string title() const override {
    return HasConst ? "Remove const from method" : "Add const to method";
  }

  bool prepare(const Selection &Sel) override {
    M = getSelectedMethod(Sel.ASTSelection.commonAncestor());

    // Bail out if the selection is not a in-line function definition.
    if (!M)
      return false;

    HasConst = M->isConst();
    return true;
  }

  Expected<Effect> apply(const Selection &Sel) override {
    return error("Not implemented");
  }

private:
  const CXXMethodDecl *M = nullptr;
  bool HasConst = false;
};

REGISTER_TWEAK(AddRemoveMethodConst)

} // namespace
} // namespace clangd
} // namespace clang
