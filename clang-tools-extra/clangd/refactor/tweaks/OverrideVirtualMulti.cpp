//===--- DefineOutline.cpp ---------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AST.h"
#include "ParsedAST.h"
#include "Selection.h"
#include "SourceCode.h"
#include "refactor/Tweak.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Attrs.inc"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <optional>
#include <string>

namespace clang {
namespace clangd {
namespace {
  static const CXXRecordDecl *selectedRecord(const Tweak::Selection &S) {
    const SourceManager &SM = S.AST->getSourceManager();

    // Use the beginning of the selection (or cursor) and map through macros.
    for (const SelectionTree::Node *N = S.ASTSelection.commonAncestor(); N; N = N->Parent) {
      if (const Decl *D = N->ASTNode.get<Decl>())
      {
        if (const auto *RD = dyn_cast<CXXRecordDecl>(D))
          return RD->getDefinition();
        return nullptr; // if we're inside anything other type of Decl but a
                        // class/struct - not our table
      }
    }

    auto &AST = *S.AST;
    auto &Ctx = AST.getASTContext();
    auto Offset= S.SelectionBegin;

    // 3) Fallback: lexical containment using the class’s brace range.
    const LangOptions &Lang = Ctx.getLangOpts();
    SourceLocation Loc = SM.getComposedLoc(SM.getMainFileID(), Offset);
    const CXXRecordDecl *Best = nullptr;
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
      const CXXRecordDecl *&Best; unsigned &BestSpan;

      bool TraverseFunctionDecl(FunctionDecl *D) {
        if (Contains(D->getSourceRange()))
        {
          Best = nullptr;
          return false;
        }
        return RecursiveASTVisitor::TraverseFunctionDecl(D);
      }
      bool TraverseCXXMethodDecl(CXXMethodDecl *D) {
        if (Contains(D->getSourceRange()))
        {
          Best = nullptr;
          return false;
        }
        return RecursiveASTVisitor::TraverseCXXMethodDecl(D);
      }
      bool TraverseCXXRecordDecl(CXXRecordDecl *RD) {
        if (RD->isThisDeclarationADefinition()) {
          SourceRange Br = RD->getBraceRange();
          if (Contains(Br)) {
            unsigned Span = SM->getFileOffset(
                Lexer::getLocForEndOfToken(Br.getEnd(), 0, *SM, *Lang)) -
                SM->getFileOffset(SM->getFileLoc(Br.getBegin()));
            if (Span < BestSpan) { BestSpan = Span; Best = RD; }
          }
        }
        return RecursiveASTVisitor::TraverseCXXRecordDecl(RD);
      }
    } V{{}, &SM, &Lang, Contains, Best, BestSpan};

    V.TraverseDecl(Ctx.getTranslationUnitDecl());
    return Best; // nullptr if not inside any class
  }
  // Build a set of canonical base methods already overridden by RD.
  static llvm::DenseSet<const CXXMethodDecl*> buildOverriddenSet(const CXXRecordDecl *RD) {
    llvm::DenseSet<const CXXMethodDecl*> S;
    if (!RD) return S;
    for (const CXXMethodDecl *M : RD->methods()) {
      for (const CXXMethodDecl *OM : M->overridden_methods())
        S.insert(OM->getCanonicalDecl());
    }
    return S;
  }
  
  struct AllVirtualMethods
  {
    const CXXMethodDecl* MainD = nullptr;
    llvm::DenseSet<const CXXMethodDecl*> AssociatedMethods;

    bool containsIn(llvm::DenseSet<const CXXMethodDecl*> const& Existing) const
    {
      for(auto const *D : AssociatedMethods)
        if (Existing.find(D) != Existing.end())
          return true;
      
      return false;
    }
  };

  // Collect all virtual methods from base classes, recursing bases.
  static void collectBaseVirtuals(const CXXRecordDecl *RD,
      SmallVectorImpl<AllVirtualMethods> &Out, llvm::DenseMap<const CXXMethodDecl*, int> &SeenMethods) {
    if (!RD) return;
    for (const auto &B : RD->bases()) {
      const Type *BT = B.getType().getTypePtrOrNull();
      if (!BT) continue;
      const auto *Base = BT->getAsCXXRecordDecl();
      if (!Base) continue;
      const CXXRecordDecl *Def = Base->getDefinition();
      if (!Def) continue;
      if (Def->isEffectivelyFinal())
        continue;
      // Add virtual methods from this base.
      for (const CXXMethodDecl *M : Def->methods()) {
        if (!M->isVirtual() || isa<CXXConstructorDecl>(M) || isa<CXXConversionDecl>(M))
          continue;
        if (M->hasAttr<clang::FinalAttr>())
          continue;

        if (const auto *Dtor = dyn_cast<CXXDestructorDecl>(M)) {
          //we don't support Dtors
          (void)Dtor;
          continue;
        }

        int Idx;
        AllVirtualMethods *AllM = nullptr;
        auto *CanonM = M->getCanonicalDecl();
        if (auto It = SeenMethods.find(CanonM); It != SeenMethods.end())
        {
          Idx = It->second;
          AllM = &Out[It->second];
        }else
        {
          Idx = (int)Out.size();
          Out.push_back({});
          AllM = &Out.back();
          AllM->AssociatedMethods.insert(CanonM);
          SeenMethods[CanonM] = Idx;
        }

        if (M->size_overridden_methods() == 0)
        {
          AllM->MainD = M;
        }
        else
        {
          for(auto *OvD : M->overridden_methods())
          {
            AllM->AssociatedMethods.insert(OvD);
            SeenMethods[OvD] = Idx;
          }
        }
      }
      // Recurse
      collectBaseVirtuals(Def, Out, SeenMethods);
    }
  }
  
  static std::string prefixBeforeNameWithMacros(const FunctionDecl* FD,
                                              const SourceManager& SM,
                                              const LangOptions& LO) {
  // Spelled location of the identifier (MethodName).
  SourceLocation Name = SM.getSpellingLoc(FD->getLocation());
  // Start of the declaration as written (attributes/virtual/return type/macros).
  SourceLocation Begin = SM.getSpellingLoc(FD->getBeginLoc());

  // Try full [Begin, Name). If that crosses macros in a way FileRange can't handle,
  // fall back to line start → name (still preserves `SPECIAL_CODE` on that line).
  auto CharRange = CharSourceRange::getCharRange(Begin, Name);
  auto Range = toHalfOpenFileRange(SM, LO, CharRange.getAsRange());
  if (!Range) {
    FileID FID = SM.getFileID(Name);
    unsigned Line = SM.getSpellingLineNumber(Name);
    SourceLocation LineStart = SM.translateLineCol(FID, Line, /*Col=*/1);
    CharRange = CharSourceRange::getCharRange(LineStart, Name);
    Range = toHalfOpenFileRange(SM, LO, CharRange.getAsRange());
  }
  if (!Range) return {};

  // Raw spelled text: includes `SPECIAL_CODE` exactly as written (no expansion).
  std::string Prefix = std::string(Lexer::getSourceText(CharRange, SM, LO));
  // Optional: strip trailing spaces.
  while (!Prefix.empty() && isspace(Prefix.back())) Prefix.pop_back();
  return Prefix;
}

static std::string afterNamePrototype(const CXXMethodDecl* MD, const ASTContext& Ctx) {
  // Print only the part after the name: (params) cv/ref/noexcept, etc.
  // Keep it simple or port the suffix builder from DefineOutline for fidelity.
  const auto* FPT = MD->getType()->getAs<FunctionProtoType>();

  std::string S; llvm::raw_string_ostream OS(S);
  OS << "(";
  for (unsigned I = 0; I < MD->getNumParams(); ++I) {
    if (I) OS << ", ";
    const auto *Param = MD->getParamDecl(I);
    Param->print(OS, Ctx.getPrintingPolicy());
    // MD->getParamDecl(i)->getType().print(OS, Ctx.getPrintingPolicy());
    // names & default args are optional; usually omit defaults in overrides
  }
  OS << ")";

  // cv-qualifiers on the implicit object:
  auto Q = FPT->getMethodQuals();
  if (Q.hasConst())    OS << " const";
  if (Q.hasVolatile()) OS << " volatile";

  // ref-qualifier:
  switch (FPT->getRefQualifier()) {
    case RQ_LValue: OS << " &"; break;
    case RQ_RValue: OS << " &&"; break;
    default: break;
  }

  // noexcept (simplified):
  if (FPT->hasNoexceptExceptionSpec()) OS << " noexcept";

  // trailing requires? (optional – add if you need it)
  return OS.str();
}

std::string makeOverrideDeclKeepingMacros(const CXXMethodDecl* BaseMD,
                                          const ParsedAST& AST) {
  const auto& SM = AST.getSourceManager();
  const auto& LO = AST.getASTContext().getLangOpts();

  std::string Prefix = prefixBeforeNameWithMacros(BaseMD, SM, LO);
  std::string Name   = BaseMD->getNameAsString();
  std::string Suf    = afterNamePrototype(BaseMD, AST.getASTContext());

  // Don’t carry `= 0` (pure) – we’re adding `override` instead.
  // (We didn’t copy any suffix text from source, so there’s no '= 0' to strip.)

  // Result keeps SPECIAL_CODE exactly as spelled:
  // e.g. "virtual void SPECIAL_CODE " + "MethodName" + "(...) const & noexcept override;"
  return Prefix + " " + Name + Suf + " override;";
}

  static std::optional<SourceLocation> declStartFromSelection(const Tweak::Selection &S, const CXXRecordDecl *TargetRD) {
    const Decl* TargetD = nullptr;
    for (const SelectionTree::Node *N = S.ASTSelection.commonAncestor(); N; N = N->Parent) {
      if (const Decl *D = N->ASTNode.get<Decl>()) {
        if (D == TargetRD)
          return std::nullopt;
        TargetD = D;
        break;
      }
    }  
    if (TargetD)
    {
      auto &AST = TargetD->getASTContext();
      auto &SM = AST.getSourceManager();
      return SM.getFileLoc(TargetD->getBeginLoc());
    }
    return std::nullopt;
  }

/// Override 1 virtual function from one of base classes
/// TODO: describe
/// Before:
/// class Base {
/// public:
///   virtual void method1(int param = 3) = 0; 
/// };
/// class Derived: public Base{
/// public:
///   <invoke here>
/// };
///
/// ----------------
///
/// After:
/// class Base ...;
/// class Derived: public Base{
/// public:
///   virtual void method1(int param = 3) override;
/// };
class OverrideVirtualMulti : public Tweak {
public:
  const char *id() const override;

  bool hidden() const override { return false; }
  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }
  std::string title() const override {
    return "Override a virtual method from a base class";
  }

  bool prepare(const Selection &Sel) override {
      const auto *RD = selectedRecord(Sel);
      if (!RD)
        return false;
      auto ExistingOverriden = buildOverriddenSet(RD);
      llvm::DenseMap<const CXXMethodDecl*, int> SeenMethods;
      SmallVector<AllVirtualMethods, 32> BaseMethods;
      collectBaseVirtuals(RD, BaseMethods, SeenMethods);
      for (const auto &BM : BaseMethods) {
        if (!BM.containsIn(ExistingOverriden))
          return true;
      }
      return false;
  }

  Expected<Effect> apply(const Selection &Sel) override {
      return error("applyMultiInvocation should be called");
  }

  bool supportsMultiple() const override 
  { 
    return true; 
  }

  std::vector<MultiInvocation> getMultipleInvocations(const Selection &Sel) override
  { 
      const auto *RD = selectedRecord(Sel);
      if (!RD)
        return {};
      auto ExistingOverriden = buildOverriddenSet(RD);
      SmallVector<AllVirtualMethods, 32> BaseMethods;
      llvm::DenseMap<const CXXMethodDecl*, int> SeenMethods;
      collectBaseVirtuals(RD, BaseMethods, SeenMethods);
      std::vector<MultiInvocation> Res;
      Res.reserve(BaseMethods.size());
      for (const auto &BM : BaseMethods) {
        if (!BM.containsIn(ExistingOverriden))
        {
          auto *D = BM.MainD->getCanonicalDecl();
          std::string TypeName = D->getQualifiedNameAsString();
          std::string Title = "Add override for ";
          Title += TypeName;
          Res.push_back(MultiInvocation{std::move(Title), std::move(TypeName)});
        }
      }
      return Res;
  }

  Expected<Effect> applyMultiInvocation(const Selection &Sel, std::string const& Param) override
  { 
      const auto *RD = selectedRecord(Sel);
      if (!RD || !RD->isThisDeclarationADefinition())
        return error("Override cannot be applied, no class or struct found or inside a method");

      auto ExistingOverriden = buildOverriddenSet(RD);
      SmallVector<AllVirtualMethods, 32> BaseMethods;
      llvm::DenseMap<const CXXMethodDecl*, int> SeenMethods;
      collectBaseVirtuals(RD, BaseMethods, SeenMethods);
      std::vector<MultiInvocation> Res;
      Res.reserve(BaseMethods.size());
      const CXXMethodDecl *TargetBM = nullptr;
      for (const auto &BM : BaseMethods) {
        if (!BM.containsIn(ExistingOverriden))
        {
          auto *D = BM.MainD->getCanonicalDecl();
          std::string TypeName = D->getQualifiedNameAsString();
          if (TypeName == Param)
          {
            TargetBM = BM.MainD;
            //found
            break;
          }
        }
      }
      if (!TargetBM)
        return error("Method to override not found");

      auto &AST = RD->getASTContext();
      auto &SM = AST.getSourceManager();
      llvm::Error Errors = llvm::Error::success();
      std::string OverrideDeclStr = makeOverrideDeclKeepingMacros(TargetBM, *Sel.AST);
      // std::string OverrideDeclStr = printOverrideDecl(TargetBM, RD, RD->getASTContext());

      auto DeclStart = declStartFromSelection(Sel, RD);
      if (!DeclStart)
        DeclStart = Sel.Cursor;
      tooling::Replacements OverrideDeclarations;
      if (auto Err = OverrideDeclarations.add(
              tooling::Replacement(SM, *DeclStart, 0, OverrideDeclStr)))
        Errors = llvm::joinErrors(std::move(Errors), std::move(Err));
      
      if (Errors)
        return std::move(Errors);

      std::optional<Path> CCFile = Sel.AST->tuPath().str();
      if (!CCFile)
        return error("Couldn't find a suitable implementation file.");
        //: getSourceFile(Sel.AST->tuPath(), Sel);
      auto Buffer = Sel.FS->getBufferForFile(*CCFile);
      auto Contents = Buffer->get()->getBuffer();
      SourceManagerForFile SMFF(*CCFile, Contents);
      auto Effect = Effect::mainFileEdit(
          SMFF.get(), OverrideDeclarations);
      if (!Effect)
        return Effect.takeError();

      return std::move(*Effect);
  };
};

REGISTER_TWEAK(OverrideVirtualMulti)

} // namespace
} // namespace clangd
} // namespace clang
