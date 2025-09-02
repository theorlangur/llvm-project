#include "AST.h"
#include "ParsedAST.h"
#include "Selection.h"
#include "SourceCode.h"
#include "TUScheduler.h"
#include "ClangdServer.h"
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
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstddef>
#include <optional>
#include <string>

//1. add support for return type change
//2. add support for +<type> <arg> syntax to add new arguments
//3. add support for virtual methods
//4. add/remove const (probably a separate tweak)

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

    static std::string getText(const SourceManager &SM, const LangOptions &Lang,
        SourceRange R) {
      CharSourceRange CR =
        CharSourceRange::getTokenRange(R.getBegin(), R.getEnd());
      return std::string(Lexer::getSourceText(CR, SM, Lang));
    }

    struct ParamDesc
    {
      int Pos;
      std::string Spelling;
      std::string Type;
      std::string Name;
      std::string Default;
    };

    std::vector<ParamDesc> getParametersFromFunctionDecl(const FunctionDecl* FD, const SourceManager &SM, const LangOptions &Lang)
    {
      std::vector<ParamDesc> Res;
      int Idx = 0;
      for(const ParmVarDecl *P : FD->parameters())
      {
        ParamDesc PD;
        PD.Pos = Idx++;
        PD.Spelling = getText(SM, Lang, P->getSourceRange());
        PD.Type = P->getType().getAsString();
        PD.Name = P->getNameAsString();
        if (P->hasDefaultArg())
        {
          if (!P->hasUnparsedDefaultArg() && !P->hasInheritedDefaultArg())
            PD.Default = getText(SM, Lang, P->getDefaultArgRange());
          else if (size_t Eq = PD.Spelling.find('='); Eq != std::string::npos)
            PD.Default = PD.Spelling.substr(Eq);
        }
        Res.push_back(std::move(PD));
      }

      return Res;
    }

    std::optional<std::vector<ParamDesc>> getParametersFromAlternativeSignature(StringRef Sig, const FunctionDecl* FD, const SourceManager &SM, const LangOptions &Lang, ClangdServer *Server, StringRef Path)
    {
      std::string Content = SM.getBufferData(SM.getMainFileID()).str();
      auto *TSI = FD->getTypeSourceInfo();
      auto TLoc = TSI->getTypeLoc();
      auto FTLoc = TLoc.getAs<FunctionTypeLoc>();
      auto LParen = FTLoc.getLParenLoc();
      auto RParen = FTLoc.getRParenLoc();
      size_t OffL = SM.getFileOffset(LParen);
      size_t OffR = SM.getFileOffset(RParen);
      std::string NewSig = "(";
      NewSig += Sig;
      NewSig += ")";
      Content.replace(OffL, OffR - OffL + 1, NewSig);

      auto FDSR = FD->getNameInfo().getSourceRange();
      volatile std::string NIName = FD->getNameInfo().getAsString();
      size_t OffFDBeg = SM.getFileOffset(FDSR.getBegin());
      size_t OffFDEnd = SM.getFileOffset(FDSR.getEnd());
      if (OffFDBeg == OffFDEnd)
      {
        //alternative 
        OffFDBeg = SM.getFileOffset(FD->getLocation());
        OffFDEnd = SM.getFileOffset(LParen);
      }
      Content.replace(OffFDBeg, OffFDEnd - OffFDBeg, "__clangd__new_sig__");

       auto AST = Server->buildAST(Path, Content);
       if (AST)
       {
         struct V : RecursiveASTVisitor<V> {
           const FunctionDecl *NewFD = nullptr;
           bool TraverseFunctionDecl(FunctionDecl *D) {
             if (D->getNameAsString() == "__clangd__new_sig__")
             {
               NewFD = D;
               return false;
             }
             return RecursiveASTVisitor::TraverseFunctionDecl(D);
           }
           bool TraverseCXXMethodDecl(CXXMethodDecl *D) {
             if (D->getNameAsString() == "__clangd__new_sig__")
             {
               NewFD = D;
               return false;
             }
             return RecursiveASTVisitor::TraverseCXXMethodDecl(D);
           }
         } V;

         auto &Ctx = AST->getASTContext();
         V.TraverseDecl(Ctx.getTranslationUnitDecl());
         if (V.NewFD)
           return getParametersFromFunctionDecl(V.NewFD, Ctx.getSourceManager(), Lang);
       }
       return std::nullopt;
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

            auto NewParams = getParametersFromAlternativeSignature(NewSignature, FD, SM, Lang, Sel.Server, Sel.AST->tuPath());
            if (!NewParams)
              return false;
            auto OldParams = getParametersFromFunctionDecl(FD, SM, Lang);
            NewParams_ = std::move(*NewParams);
            //match old with new (name based), best effort
            //the rest as-is
            for(auto &P : NewParams_)
            {
              P.Pos = -1;
              for(size_t Idx = 0, N = OldParams.size(); Idx < N; ++Idx)
              {
                if ((P.Name == OldParams[Idx].Name) && (P.Type == OldParams[Idx].Type))
                {
                  P.Pos = Idx;
                  break;
                }
              }
            }
            return true;
          }

          if (Comment.starts_with("+"))
          {
            StringRef NewParamsStr = Comment.drop_front(1);
            auto OldParams = getParametersFromFunctionDecl(FD, SM, Lang);
            for(auto const& P : OldParams)
            {
              if (!NewSignature_.empty())
                NewSignature_ += ", ";
              NewSignature_ += P.Spelling;
            }
            NewSignature_ += ", ";
            NewSignature_ += NewParamsStr;
            auto NewParams = getParametersFromAlternativeSignature(NewSignature_, FD, SM, Lang, Sel.Server, Sel.AST->tuPath());
            if (!NewParams)
              return false;
            NewParams_ = std::move(*NewParams);
            return true;
          }

          return false;
        }

        tooling::Replacement updateDeclaration(SourceLocation LParen, SourceLocation RParen, const SourceManager &SM)
        {
          CharSourceRange DelRange = CharSourceRange::getTokenRange(LParen, RParen);
          std::string Sig;
          Sig.reserve(NewSignature_.size() * 2);
          for(auto const& P : NewParams_)
          {
            if (Sig.empty())
            {
              Sig = "(";
            }else
            {
              Sig += ", ";
            }
            Sig += P.Spelling;
            //Sig += P.Type;
            //Sig += ' ';
            //Sig += P.Name;
          }
          Sig += ")";
          return tooling::Replacement(SM, DelRange, Sig);
        }

        tooling::Replacement updateDefinition(SourceLocation LParen, SourceLocation RParen, const SourceManager &SM)
        {
          CharSourceRange DelRange = CharSourceRange::getTokenRange(LParen, RParen);
          std::string Sig;
          Sig.reserve(NewSignature_.size() * 2);
          for(auto const& P : NewParams_)
          {
            if (Sig.empty())
            {
              Sig = "(";
            }else
            {
              Sig += ", ";
            }
            Sig += P.Type;
            Sig += ' ';
            Sig += P.Name;
          }
          Sig += ")";
          return tooling::Replacement(SM, DelRange, Sig);
        }

        tooling::Replacement updateDeclaration(const FunctionDecl* FD, const SourceManager &SM)
        {
          auto *TSI = FD->getTypeSourceInfo();
          auto TLoc = TSI->getTypeLoc();
          auto FTLoc = TLoc.getAs<FunctionTypeLoc>();
          auto LParen = FTLoc.getLParenLoc();
          auto RParen = FTLoc.getRParenLoc();
          return updateDeclaration(LParen, RParen, SM);
        }

        struct LexEntry
        {
          LexEntry(SourceManagerForFile &&_SMF, std::vector<syntax::Token> &&_Tokens, std::unique_ptr<llvm::MemoryBuffer> &&_MB): 
            SMF(std::move(_SMF)), Tokens(std::move(_Tokens)), MB(std::move(_MB)){}
          SourceManagerForFile SMF;
          std::vector<syntax::Token> Tokens;
          std::unique_ptr<llvm::MemoryBuffer> MB;
        };

        Expected<Effect> apply(const Selection &Sel) override {
          const FunctionDecl *FD = selectedFunctionDecl(Sel);
          if (!FD)
            return error("No function/method declaration found");

          if (!NewParams_.empty())
          {
            //validate defaults
            bool ExpectedDefault = false;
            for(auto const& P : NewParams_)
            {
              if (P.Default.empty() && ExpectedDefault)
              {
                std::string Err = "Parameter ";
                Err += P.Spelling;
                Err += " must have a default";
                return error(Err);
              }
              if (!P.Default.empty())
                ExpectedDefault = true;
            }
          }

          Tweak::Effect Effect;

          struct PerFileEdits
          {
            tooling::Replacements Replacements;
            std::unique_ptr<LexEntry> LE;
          };
          auto &AST = *Sel.AST;
          auto &Ctx = AST.getASTContext();
          const LangOptions &Lang = Ctx.getLangOpts();
          std::unordered_map<std::string, PerFileEdits> ASTs;
          RefsRequest Refs;
          llvm::Error Errors = llvm::Error::success();
          Refs.IDs.insert(getSymbolID(FD));
          Refs.Filter = RefKind::Declaration | RefKind::Definition | RefKind::Call;
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
                  decltype(&ChangeSignature::changeDeclWithLexer) ChangeMethod = nullptr;
                  if ((R.Kind & (RefKind::Definition)) != RefKind::Unknown)
                    ChangeMethod = &ChangeSignature::changeDefWithLexer;
                  else if ((R.Kind & (RefKind::Declaration)) != RefKind::Unknown)
                    ChangeMethod = &ChangeSignature::changeDeclWithLexer;
                  else if ((R.Kind & RefKind::Call) != RefKind::Unknown)
                    ChangeMethod = &ChangeSignature::changeCallWithLexer;

                  if (ChangeMethod)
                  {
                    if (auto R = (this->*ChangeMethod)(*ASTIt->second.LE, Lang, L))
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

          Effect.FormatEdits = false;
          return Effect;
        }

        std::unique_ptr<LexEntry> prepareLexerFor(StringRef Path, std::unique_ptr<llvm::MemoryBuffer> MB, const LangOptions &Lang)
        {
          SourceManagerForFile FileSM(Path, MB->getBuffer());
          auto &SM = FileSM.get();
          auto Tokens = syntax::tokenize(SM.getMainFileID(), SM, Lang);
          return std::make_unique<LexEntry>(std::move(FileSM), std::move(Tokens), std::move(MB));
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

        std::optional<tooling::Replacement> changeDeclWithLexer(LexEntry &LE, const LangOptions &Lang, SymbolLocation const& L)
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
                    SigEnd = Tok.location();
                    break;
                  }
                }
              }
              break;
            }
          }
          return updateDeclaration(SigStart, SigEnd, SM);
        }

        std::optional<tooling::Replacement> changeDefWithLexer(LexEntry &LE, const LangOptions &Lang, SymbolLocation const& L)
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
                    SigEnd = Tok.location();
                    break;
                  }
                }
              }
              break;
            }
          }
          return updateDefinition(SigStart, SigEnd, SM);
        }

        std::optional<tooling::Replacement> changeCallWithLexer(LexEntry &LE, const LangOptions &Lang, SymbolLocation const& L)
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

          std::vector<std::string> CallArguments;
          SourceLocation CallStart, CallEnd;
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
                CallStart = Tokens[Index].location();
                SourceLocation ArgStart, ArgEnd;
                ++Pairs;
                for(++Index;Index < Last; ++Index)
                {
                  const auto &Tok = Tokens[Index];
                  if (Tok.kind() == tok::TokenKind::comma)
                  {
                      if (ArgStart.isValid())//could be asserted
                      {
                        if (Tokens[Index - 1].location() == ArgStart)
                          ArgEnd = Tokens[Index - 1].endLocation();
                        else
                          ArgEnd = Tokens[Index - 1].location();
                        CharSourceRange CSR = CharSourceRange::getCharRange(ArgStart, ArgEnd);
                        CallArguments.emplace_back(Lexer::getSourceText(CSR, SM, Lang));
                        ArgStart = {};
                      }
                  }else
                    balanceParenTypes(Tok, Pairs);

                  if (!Pairs)
                  {
                    CallEnd = Tok.location();
                    if (ArgStart.isValid())
                    {
                        if (Tokens[Index - 1].location() == ArgStart)
                          ArgEnd = Tokens[Index - 1].endLocation();
                        else
                          ArgEnd = Tokens[Index - 1].location();
                        CharSourceRange CSR = CharSourceRange::getCharRange(ArgStart, ArgEnd);
                        CallArguments.emplace_back(Lexer::getSourceText(CSR, SM, Lang));
                    }
                    break;
                  }
                  if ((Tok.kind() != tok::TokenKind::comma) && ArgStart.isInvalid())
                  {
                    ArgStart = Tok.location();
                  }
                }
              }
              break;
            }
          }

          std::string NewSig;
          for(int I = 0, N = (int)NewParams_.size(); I < N; ++I)
          {
            if (NewSig.empty())
            {
              NewSig = "(";
              if (!NewParams_[I].Default.empty() && I >= (int)CallArguments.size()) //no need to continue, the rest can be 'defaults'
                break;
            }else
            {
              if (!NewParams_[I].Default.empty() && I >= (int)CallArguments.size()) //no need to continue, the rest can be 'defaults'
                break;
              NewSig += ", ";
            }
            if (NewParams_[I].Pos != -1 && I < (int)CallArguments.size())
            {
              NewSig += CallArguments[I];
            }else
            {
              NewSig += "/*";
              NewSig += NewParams_[I].Name;
              NewSig += "*/";
            }
          }
          NewSig += ")";

          CharSourceRange DelRange = CharSourceRange::getTokenRange(CallStart, CallEnd);
          return tooling::Replacement(SM, DelRange, NewSig);
        }

      private:
        std::string NewSignature_;
        std::vector<ParamDesc> NewParams_;
    };

    REGISTER_TWEAK(ChangeSignature)

    } // namespace
  } // namespace clangd
} // namespace clang
