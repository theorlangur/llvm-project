//===--- DefineOutline.cpp ---------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AST.h"
#include "FindTarget.h"
#include "HeaderSourceSwitch.h"
#include "ParsedAST.h"
#include "Selection.h"
#include "SourceCode.h"
#include "index/Index.h"
#include "refactor/Tweak.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "unittests/TestIndex.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Attrs.inc"
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

  static const CXXRecordDecl *selectedRecord(const Tweak::Selection &S) {
    const SourceManager &SM = S.AST->getSourceManager();
    const LangOptions &LangOpts = S.AST->getASTContext().getLangOpts();

    // Use the beginning of the selection (or cursor) and map through macros.
    SourceLocation Cur = SM.getFileLoc(S.Cursor);

    for (const SelectionTree::Node *N = S.ASTSelection.commonAncestor(); N; N = N->Parent) {
      if (const Decl *D = N->ASTNode.get<Decl>())
      {
        if (const auto *FD = dyn_cast<FunctionDecl>(D))
        {
          //we don't accept cursors inside function bodies
          if (FD->doesThisDeclarationHaveABody())
          {
            auto *Body = FD->getBody();
            if (Body)
            {
              // Compute an inclusive range for the body { ... }.
              SourceLocation B = SM.getFileLoc(Body->getBeginLoc());
              SourceLocation E = SM.getFileLoc(Body->getEndLoc());
              if (B.isValid() && E.isValid())
              {
                // End-of-token so the closing '}' is included.
                SourceLocation ETok =
                  Lexer::getLocForEndOfToken(E, /*Offset=*/0, SM, LangOpts);

                bool AfterBegin = !SM.isBeforeInTranslationUnit(Cur, B);
                bool BeforeEnd  =  SM.isBeforeInTranslationUnit(Cur, ETok);
                if (AfterBegin && BeforeEnd)
                  return nullptr;
              }
            }
          }
        }
        if (const auto *RD = dyn_cast<CXXRecordDecl>(D))
          return RD->getDefinition();
      }
    }
    return nullptr;
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

  // Collect all virtual methods from base classes, recursing bases.
  static void collectBaseVirtuals(const CXXRecordDecl *RD,
      SmallVectorImpl<const CXXMethodDecl*> &Out) {
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
        Out.push_back(M->getCanonicalDecl());
      }
      // Recurse
      collectBaseVirtuals(Def, Out);
    }
  }

  // Very simple text printer for an override *declaration* with `override`.
  static std::string printOverrideDecl(const CXXMethodDecl *BaseMD,
      const CXXRecordDecl *Derived,
      ASTContext &Ctx) {
    PrintingPolicy PP(Ctx.getPrintingPolicy());
    PP.SuppressScope = true; // no qualification inside class
    PP.FullyQualifiedName = false;


    std::string Sig;
    llvm::raw_string_ostream OS(Sig);


    // Derive the method type as seen in Derived; this is a simplification.
    // For accuracy, use DefineOutline-like printers.
    const FunctionProtoType *FPT = BaseMD->getType()->getAs<FunctionProtoType>();
    (void)FPT; // placeholder
    BaseMD->print(OS, PP);
    if (!Sig.empty() && Sig.back() == ';') Sig.pop_back();
    OS.flush();


    // Ensure we drop any '= 0' and add 'override;'
    size_t PurePos = Sig.find("= 0");
    if (PurePos != std::string::npos)
      Sig.erase(PurePos);
    // Remove possible trailing spaces
    while (!Sig.empty() && isspace(Sig.back())) Sig.pop_back();


    Sig.append(" override;");
    return Sig;
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
      SmallVector<const CXXMethodDecl*, 32> BaseMethods;
      collectBaseVirtuals(RD, BaseMethods);
      for (const CXXMethodDecl *BM : BaseMethods) {
        //if (BM->isFinal()) continue; // can't override final methods
        if (!ExistingOverriden.count(BM->getCanonicalDecl()))
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
      SmallVector<const CXXMethodDecl*, 32> BaseMethods;
      collectBaseVirtuals(RD, BaseMethods);
      std::vector<MultiInvocation> Res;
      Res.reserve(BaseMethods.size());
      for (const CXXMethodDecl *BM : BaseMethods) {
        auto *D = BM->getCanonicalDecl();
        //if (BM->isFinal()) continue; // can't override final methods
        if (!ExistingOverriden.count(BM->getCanonicalDecl()))
        {
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
      SmallVector<const CXXMethodDecl*, 32> BaseMethods;
      collectBaseVirtuals(RD, BaseMethods);
      std::vector<MultiInvocation> Res;
      Res.reserve(BaseMethods.size());
      const CXXMethodDecl *TargetBM = nullptr;
      for (const CXXMethodDecl *BM : BaseMethods) {
        auto *D = BM->getCanonicalDecl();
        //if (BM->isFinal()) continue; // can't override final methods
        if (!ExistingOverriden.count(D))
        {
          std::string TypeName = D->getQualifiedNameAsString();
          if (TypeName == Param)
          {
            TargetBM = BM;
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

      std::string OverrideDeclStr = printOverrideDecl(TargetBM, RD, RD->getASTContext());

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
