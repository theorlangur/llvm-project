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
#include "clang/AST/Type.h"
#include "clang/AST/RecursiveASTVisitor.h"
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
  static const FunctionDecl *selectedFunctionDecl(const Tweak::Selection &S) {
    const SourceManager &SM = S.AST->getSourceManager();

    // Use the beginning of the selection (or cursor) and map through macros.
    for (const SelectionTree::Node *N = S.ASTSelection.commonAncestor(); N; N = N->Parent) {
      if (const FunctionDecl *D = N->ASTNode.get<FunctionDecl>())
        return D;
    }

    auto &AST = *S.AST;
    auto &Ctx = AST.getASTContext();
    auto Offset= S.SelectionBegin;

    // 3) Fallback: lexical containment using the class’s brace range.
    const LangOptions &Lang = Ctx.getLangOpts();
    SourceLocation Loc = SM.getComposedLoc(SM.getMainFileID(), Offset);
    const FunctionDecl *Best = nullptr;
    unsigned BestSpan = std::numeric_limits<unsigned>::max();

    auto Contains = [&](SourceRange R) {
      if (!R.isValid()) return false;
      SourceLocation B = SM.getFileLoc(R.getBegin());
      SourceLocation E = Lexer::getLocForEndOfToken(SM.getFileLoc(R.getEnd()), 0, SM, Lang);
      return !SM.isBeforeInTranslationUnit(Loc, B) &&
              SM.isBeforeInTranslationUnit(Loc, E);
    };

    struct V : RecursiveASTVisitor<V> {
      const SourceManager *SM; 
      const LangOptions *Lang;
      std::function<bool(SourceRange)> Contains;
      const FunctionDecl *&Best; unsigned &BestSpan;

      bool TraverseFunctionDecl(FunctionDecl *D) {
        if (Contains(D->getSourceRange())) {
          Best = D;
          return false;
        }
        return RecursiveASTVisitor::TraverseFunctionDecl(D);
      }
      bool TraverseCXXMethodDecl(CXXMethodDecl *D) {
        if (Contains(D->getSourceRange())) {
          Best = D;
          return false;
        }
        return RecursiveASTVisitor::TraverseCXXMethodDecl(D);
      }
    } V{{}, &SM, &Lang, Contains, Best, BestSpan};

    V.TraverseDecl(Ctx.getTranslationUnitDecl());
    return Best; // nullptr if not inside any function decl
  }

/// Changes the signature of a selected function/method according to a new declaration in the comment block
/// TODO: povide a proper example
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
class ChangeSignature : public Tweak {
public:
  const char *id() const override;

  bool hidden() const override { return false; }
  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }
  std::string title() const override {
    return std::string("Change a function/method signature to ") + NewSignature_;
  }

  bool prepare(const Selection &Sel) override {

    const FunctionDecl *FD = selectedFunctionDecl(Sel);
    if (!FD)
      return false;
    const SourceManager &SM = Sel.AST->getSourceManager();
    SourceLocation SelBegin = SM.getComposedLoc(SM.getMainFileID(), Sel.SelectionBegin);
    if (SelBegin.isInvalid())
      return false;

    auto FDBegin = FD->getBeginLoc();
    auto FDEnd = FD->getEndLoc();
    if (!SM.isBeforeInTranslationUnit(FDBegin, SelBegin)
        || !SM.isBeforeInTranslationUnit(SelBegin, FDEnd))
      return false;

    auto &AST = *Sel.AST;
    auto &Ctx = AST.getASTContext();
    const LangOptions &Lang = Ctx.getLangOpts();
    Token T;
    auto TokBegin = Lexer::GetBeginningOfToken(SelBegin, SM, Lang);
    if (Lexer::getRawToken(TokBegin, T, SM, Lang))
      return false;

    if (!T.is(tok::comment))
      return false;

    auto TokEnd = Lexer::getLocForEndOfToken(TokBegin, 0, SM, Lang);
    StringRef Comment = Lexer::getSourceText(CharSourceRange::getCharRange(TokBegin, TokEnd), SM, Lang);

    if (!Comment.starts_with("/*"))
      return false;//only block is supported

    Comment = Comment.drop_front(2).drop_back(2);

    if (Comment.starts_with("="))
    {
      StringRef NewSignature = Comment.drop_front(1);
      NewSignature_ = std::string(NewSignature.data(), NewSignature.size());
      //TODO: pre-parse here changes
      return true;
    }

    return false;
  }

  Expected<Effect> apply(const Selection &Sel) override {
    return error("not implemented");
  }
private:
  std::string NewSignature_;
};

REGISTER_TWEAK(ChangeSignature)

} // namespace
} // namespace clangd
} // namespace clang
