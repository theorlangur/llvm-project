#include "AST.h"
#include "CommonTweakTools.h"
#include "ParsedAST.h"
#include "Selection.h"
#include "SourceCode.h"
#include "index/Index.h"
#include "index/SymbolLocation.h"
#include "refactor/Tweak.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <optional>
#include <string>

namespace clang {
namespace clangd {
namespace {
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
///   void foo() const { return; }
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

    if (!M)
      return false;

    HasConst = M->isConst();
    return true;
  }

  Expected<Effect> apply(const Selection &Sel) override {

    Tweak::Effect Effect;
    auto &AST = *Sel.AST;
    auto &Ctx = AST.getASTContext();
    const LangOptions &Lang = Ctx.getLangOpts();
    std::unordered_map<std::string, PerFileEdits> ASTs;
    RefsRequest Refs;
    llvm::Error Errors = llvm::Error::success();
    Refs.IDs.insert(getSymbolID(M));
    if (M->isVirtual())
      collectAllRelatedVirtuals(M, Refs ,Sel.Index);

    Refs.Filter = RefKind::Declaration | RefKind::Definition;
    Sel.Index->refs(Refs, [&](const Ref& R)->void{
        auto L = R.Location;
        auto Path = URI::resolve(L.FileURI, Sel.AST->tuPath());
        if (Path)
        {
          auto ASTIt = ASTs.find(*Path);
          if (ASTIt == ASTs.end())
          {
            auto Buffer = Sel.FS->getBufferForFile(*Path);
            if (Buffer)
            {
              auto LE = prepareLexerFor(*Path, std::move(*Buffer), Lang);
              ASTs[*Path].LE = std::move(LE);
              ASTIt = ASTs.find(*Path);
            }
          }

          if (ASTIt != ASTs.end())
          {
            std::optional<tooling::Replacement> R;
            if (HasConst)
              R = removeConstWithLexer(*ASTIt->second.LE, Lang, L);
            else
              R = addConstWithLexer(*ASTIt->second.LE, Lang, L);

            if (R)
            {
              if (auto Err = ASTIt->second.Replacements.add(*R))
              {
                Errors = llvm::joinErrors(
                    std::move(Errors),
                    std::move(Err));
              }
            }
          }
        }
    });

    for(auto &[path, entry] : ASTs)
    {
      auto &Replacements = entry.Replacements;
      Edit Ed(entry.LE->MB->getBuffer(), std::move(Replacements));
      Effect.ApplyEdits.try_emplace(path,
          std::move(Ed));
    }

    if (Errors)
      return Errors;

    //Effect.FormatEdits = false;
    return Effect;
  }

  std::optional<tooling::Replacement> addConstWithLexer(LexEntry &LE, const LangOptions &Lang, SymbolLocation const& L)
  {
    auto &SM = LE.SMF.get();

    SourceLocation TargetStart;
    unsigned OffsetStart;
    if (auto Sz = clangd::positionToOffset(LE.MB->getBuffer(), {(int)L.Start.line(), (int)L.Start.column()}))
    {
      TargetStart = SM.getComposedLoc(SM.getMainFileID(), *Sz);
      OffsetStart = *Sz;
    }

    if (TargetStart.isInvalid())
      return std::nullopt;

    SourceLocation SigStart, SigEnd;
    const auto &Tokens = LE.Tokens;
    unsigned Last = Tokens.size() - 1;
    for (unsigned Index = 0; Index < Last; ++Index) {
      const auto &Tok = Tokens[Index];
      if (Tok.range(SM).contains(OffsetStart))
      {
        unsigned Pairs = 0;
        ++Index;
        if (Tokens[Index].kind() == tok::TokenKind::l_paren)
        {
          SigStart = Tokens[Index].location();
          ++Pairs;
          for(++Index;Index < Last; ++Index)
          {
            const auto &Tok = Tokens[Index];
            balanceParenTypes(Tok, Pairs);
            if (!Pairs)
            {
              SigEnd = Tok.endLocation();
              break;
            }
          }
        }
        break;
      }
    }

    if (SigEnd.isInvalid())
      return std::nullopt;
    CharSourceRange DelRange = CharSourceRange::getTokenRange(SigEnd, SigEnd);
    return tooling::Replacement(SM, DelRange, " const");
  }

  std::optional<tooling::Replacement> removeConstWithLexer(LexEntry &LE, const LangOptions &Lang, SymbolLocation const& L)
  {
    auto &SM = LE.SMF.get();

    SourceLocation TargetStart;
    unsigned OffsetStart;
    if (auto Sz = clangd::positionToOffset(LE.MB->getBuffer(), {(int)L.Start.line(), (int)L.Start.column()}))
    {
      TargetStart = SM.getComposedLoc(SM.getMainFileID(), *Sz);
      OffsetStart = *Sz;
    }

    if (TargetStart.isInvalid())
      return std::nullopt;

    SourceLocation SigStart, SigEnd;
    const syntax::Token *ConstTok = nullptr;
    const auto &Tokens = LE.Tokens;
    unsigned Last = Tokens.size() - 1;
    for (unsigned Index = 0; Index < Last; ++Index) {
      const auto &Tok = Tokens[Index];
      if (Tok.range(SM).contains(OffsetStart))
      {
        unsigned Pairs = 0;
        ++Index;
        if (Tokens[Index].kind() == tok::TokenKind::l_paren)
        {
          SigStart = Tokens[Index].location();
          ++Pairs;
          for(++Index;Index < Last; ++Index)
          {
            const auto &Tok = Tokens[Index];
            balanceParenTypes(Tok, Pairs);
            if (!Pairs)
            {
              SigEnd = Tok.location();
              break;
            }
          }
          if (SigEnd.isValid())
          {
            //traverse next tokens until we meet ';' or '{'
            for(++Index;Index < Last; ++Index)
            {
              const auto &Tok = Tokens[Index];
              if (Tok.kind() == tok::TokenKind::semi || Tok.kind() == tok::TokenKind::l_brace)
                break;
              if (Tok.kind() == tok::TokenKind::kw_const)
              {
                ConstTok = &Tok;
                break;
              }
            }
          }
        }
        break;
      }
    }

    if (!ConstTok)
      return std::nullopt;
    CharSourceRange DelRange = CharSourceRange::getTokenRange(ConstTok->location(), ConstTok->endLocation());
    return tooling::Replacement(SM, DelRange, "");
  }

private:
  const CXXMethodDecl *M = nullptr;
  bool HasConst = false;
};

REGISTER_TWEAK(AddRemoveMethodConst)

} // namespace
} // namespace clangd
} // namespace clang
