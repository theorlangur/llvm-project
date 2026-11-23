#ifndef common_tweak_tools_h_
#define common_tweak_tools_h_
#include "AST.h"
#include "index/Index.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clang {
namespace clangd {
namespace {
    struct LexEntry
    {
        LexEntry(SourceManagerForFile &&_SMF, std::vector<syntax::Token> &&_Tokens, std::unique_ptr<llvm::MemoryBuffer> &&_MB): 
            SMF(std::move(_SMF)), Tokens(std::move(_Tokens)), MB(std::move(_MB)){}
        SourceManagerForFile SMF;
        std::vector<syntax::Token> Tokens;
        std::unique_ptr<llvm::MemoryBuffer> MB;
    };

    struct PerFileEdits
    {
        tooling::Replacements Replacements;
        std::unique_ptr<LexEntry> LE;
    };

    void collectAllRelatedBaseVirtuals(CXXMethodDecl const* M, RefsRequest &Refs)
    {
      for(const auto *OM : M->overridden_methods())
      {
        Refs.IDs.insert(getSymbolID(OM));
        collectAllRelatedBaseVirtuals(OM, Refs);
      }
    }

    void collectAllRelatedVirtuals(CXXMethodDecl const* M, RefsRequest &Refs, const SymbolIndex *Index)
    {
      collectAllRelatedBaseVirtuals(M, Refs);

      RelationsRequest Req;
      Req.Predicate = RelationKind::OverriddenBy;
      Req.Subjects = Refs.IDs;//copy
      Index->relations(Req, [&](const SymbolID &Subject, const Symbol &Object) {
          Refs.IDs.insert(Object.ID);
      });
    }

    bool balanceParenTypes(clang::syntax::Token const &Tok, unsigned &Pairs)
    {
      switch(Tok.kind())
      {
        case tok::TokenKind::l_paren:
        case tok::TokenKind::l_brace:
        case tok::TokenKind::l_square:
        case tok::TokenKind::less:
          ++Pairs;
          return true;
        case tok::TokenKind::r_paren:
        case tok::TokenKind::r_brace:
        case tok::TokenKind::r_square:
        case tok::TokenKind::greater:
          --Pairs;
          return true;
        default:
          return false;
      }
    }

    std::unique_ptr<LexEntry> prepareLexerFor(StringRef Path, std::unique_ptr<llvm::MemoryBuffer> MB, const LangOptions &Lang)
    {
      SourceManagerForFile FileSM(Path, MB->getBuffer());
      auto &SM = FileSM.get();
      auto Tokens = syntax::tokenize(SM.getMainFileID(), SM, Lang);
      return std::make_unique<LexEntry>(std::move(FileSM), std::move(Tokens), std::move(MB));
    }
}
}
}
#endif
