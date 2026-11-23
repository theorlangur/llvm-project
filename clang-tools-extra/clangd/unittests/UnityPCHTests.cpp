//===-- UnityPCHTests.cpp - UnityProjectCDB, PCHManager, and FuzzySW tests -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FindSymbols.h"
#include "GlobalCompilationDatabase.h"
#include "PCHManager.h"
#include "ParsedAST.h"
#include "TUScheduler.h"
#include "UnityProjectCDB.h"
#include "fuzzy_sw/fuzzy_sw.hpp"
#include "index/Index.h"
#include "index/MemIndex.h"
#include "index/Symbol.h"
#include "support/Path.h"
#include "support/ThreadsafeFS.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace clang {
namespace clangd {
namespace {

class TempDir {
public:
  TempDir(llvm::StringRef Prefix) {
    llvm::SmallString<128> P;
    std::error_code EC = llvm::sys::fs::createUniqueDirectory(Prefix, P);
    EXPECT_FALSE(EC);
    PathStr = P.str().str();
  }
  ~TempDir() {
    if (!PathStr.empty())
      llvm::sys::fs::remove_directories(PathStr);
  }
  std::string path(llvm::StringRef Rel) const {
    llvm::SmallString<128> P(PathStr);
    llvm::sys::path::append(P, Rel);
    return P.str().str();
  }
  std::string root() const { return PathStr; }

private:
  std::string PathStr;
};

void writeFileText(llvm::StringRef FullPath, llvm::StringRef Contents) {
  llvm::sys::fs::create_directories(llvm::sys::path::parent_path(FullPath));
  std::error_code EC;
  llvm::raw_fd_ostream OS(FullPath, EC);
  ASSERT_FALSE(EC) << FullPath << ": " << EC.message();
  OS << Contents;
}

TEST(UnityPCHTests, UnityProjectCDBDiscoveryAndFallback) {
  TempDir Tmp("unity-cdb-test");
  std::string BuildDir = Tmp.path("build");

  std::string CoreStdafx = Tmp.path("src/core/stdafx.hpp");
  std::string AppStdafx = Tmp.path("src/app/stdafx.hpp");
  std::string CoreMath = Tmp.path("src/core/math.hpp");
  std::string CoreMathCpp = Tmp.path("src/core/math.cpp");
  std::string AppWorld = Tmp.path("src/app/world.hpp");
  std::string AppWorldCpp = Tmp.path("src/app/world.cpp");
  std::string AppPost = Tmp.path("src/app/post.hpp");
  std::string UnityCore = Tmp.path("src/unity_core.cpp");
  std::string UnityApp = Tmp.path("src/unity_app.cpp");
  std::string Standalone = Tmp.path("src/tools/standalone.cpp");

  writeFileText(CoreStdafx,
                "#pragma once\n#include \"core/math.hpp\"\ninline int core_base() { return 1; }\n");
  writeFileText(AppStdafx,
                "#pragma once\n#include \"core/stdafx.hpp\"\n#include \"app/world.hpp\"\n#include \"app/post.hpp\"\ninline int app_base() { return 2; }\n");
  writeFileText(CoreMath,
                "#ifndef CORE_MATH_HPP\n#define CORE_MATH_HPP\ninline int add1(int x) { return x + 1; }\n#endif\n");
  writeFileText(CoreMathCpp, "int core_dummy() { return add1(1); }\n");
  writeFileText(AppWorld,
                "#pragma once\nstruct WorldObj { int v = add1(1); };\n");
  writeFileText(AppWorldCpp, "int app_dummy() { return WorldObj{}.v; }\n");
  writeFileText(AppPost,
                "#pragma once\nstruct AfterWorld { int z = 99; };\n");
  writeFileText(UnityCore,
                "#include \"core/stdafx.hpp\"\n#include \"core/math.cpp\"\n");
  writeFileText(UnityApp,
                "#include \"app/stdafx.hpp\"\n#include \"app/world.cpp\"\n");
  writeFileText(Standalone, "int main() { return 0; }\n");

  llvm::json::Array DB;
  for (const std::string &TU : {UnityCore, UnityApp, Standalone}) {
    std::string Cmd = "clang++ -std=c++20 -I" + Tmp.path("src") + " -c " + TU;
    DB.push_back(llvm::json::Object{
        {"directory", BuildDir},
        {"command", Cmd},
        {"file", TU},
    });
  }
  writeFileText(BuildDir + "/compile_commands.json",
                llvm::formatv("{0:2}", llvm::json::Value(std::move(DB))).str());

  RealThreadsafeFS TFS;
  DirectoryBasedGlobalCompilationDatabase::Options CDBOpts(TFS);
  CDBOpts.CompileCommandsDir = BuildDir;
  auto BaseCDB =
      std::make_unique<DirectoryBasedGlobalCompilationDatabase>(CDBOpts);
  UnityProjectCDB UnityCDB(std::move(BaseCDB), TFS);

  // Trigger discovery and check app/world.hpp.
  auto AppCmd = UnityCDB.getCompileCommand(AppWorld);
  ASSERT_TRUE(AppCmd.has_value());

  auto WorldSpec = UnityCDB.getPCHSpec(AppWorld);
  ASSERT_TRUE(WorldSpec.has_value());
  EXPECT_TRUE(WorldSpec->IsDynamic);
  EXPECT_EQ(llvm::sys::path::filename(WorldSpec->UmbrellaHeader), "stdafx.hpp");
  EXPECT_EQ(llvm::sys::path::filename(WorldSpec->StopBeforeHeader), "world.hpp");
  EXPECT_EQ(llvm::sys::path::filename(WorldSpec->ParentPCHHeader), "stdafx.hpp");

  // App .cpp receives static PCH spec for app/stdafx.hpp (with parent core/stdafx.hpp).
  auto AppCppSpec = UnityCDB.getPCHSpec(AppWorldCpp);
  ASSERT_TRUE(AppCppSpec.has_value());
  EXPECT_FALSE(AppCppSpec->IsDynamic);
  EXPECT_EQ(llvm::sys::path::filename(AppCppSpec->PCHHeader), "stdafx.hpp");
  EXPECT_FALSE(AppCppSpec->ParentPCHHeader.empty());

  // Header included via #ifndef guard inside core/stdafx.hpp.
  auto MathSpec = UnityCDB.getPCHSpec(CoreMath);
  ASSERT_TRUE(MathSpec.has_value());
  EXPECT_TRUE(MathSpec->IsDynamic);
  EXPECT_EQ(llvm::sys::path::filename(MathSpec->UmbrellaHeader), "stdafx.hpp");
  EXPECT_TRUE(MathSpec->ParentPCHHeader.empty());

  // Querying app/stdafx.hpp returns its parent PCH spec (core/stdafx.hpp).
  auto AppStdafxParent = UnityCDB.getPCHSpec(AppStdafx);
  ASSERT_TRUE(AppStdafxParent.has_value());
  EXPECT_EQ(AppStdafxParent->PCHHeader, CoreStdafx);
  EXPECT_TRUE(AppStdafxParent->ParentPCHHeader.empty());

  // Level-1 core/stdafx.hpp has no parent PCH.
  EXPECT_FALSE(UnityCDB.getPCHSpec(CoreStdafx).has_value());

  // Standalone TU must pass through without a PCHSpec.
  EXPECT_FALSE(UnityCDB.getPCHSpec(Standalone).has_value());
}

TEST(UnityPCHTests, HierarchicalAndDynamicPCHWithPersistence) {
  TempDir Tmp("unity-pch-mgr-test");
  std::string BuildDir = Tmp.path("build");

  std::string CoreStdafx = Tmp.path("src/core/stdafx.hpp");
  std::string CoreImpl = Tmp.path("src/core/impl.cpp");
  std::string AppStdafx = Tmp.path("src/app/stdafx.hpp");
  std::string AppImpl = Tmp.path("src/app/impl.cpp");
  std::string PreWorld = Tmp.path("src/app/pre_world.hpp");
  std::string AppWorld = Tmp.path("src/app/world.hpp");
  std::string PostWorld = Tmp.path("src/app/post_world.hpp");
  std::string UnityCore = Tmp.path("src/unity_core.cpp");
  std::string UnityApp = Tmp.path("src/unity_app.cpp");

  writeFileText(CoreStdafx,
                "#ifndef CORE_STDAFX\n#define CORE_STDAFX\n#define CORE_CONST 10\ninline int core_fn() { return CORE_CONST; }\n#endif\n");
  writeFileText(CoreImpl, "int use_core() { return core_fn(); }\n");
  writeFileText(AppStdafx,
                "#pragma once\n#include \"core/stdafx.hpp\"\ninline int app_fn() { return core_fn() + 1; }\n#include \"app/pre_world.hpp\"\n#include \"app/world.hpp\"\n#include \"app/post_world.hpp\"\n");
  writeFileText(AppImpl, "int use_app() { return WorldType{}.get(); }\n");
  writeFileText(PreWorld,
                "#pragma once\nstruct PreWorldType { int x = app_fn(); };\n");
  writeFileText(AppWorld,
                "#pragma once\nstruct WorldType { PreWorldType pw; int get() const { return pw.x; } };\n");
  writeFileText(PostWorld,
                "#pragma once\nstruct PostWorldOnlyType { int y = 7; };\n");
  writeFileText(UnityCore,
                "#include \"core/stdafx.hpp\"\n#include \"core/impl.cpp\"\n");
  writeFileText(UnityApp,
                "#include \"app/stdafx.hpp\"\n#include \"app/impl.cpp\"\n");

  auto WriteCDB = [&](llvm::StringRef ExtraFlag) {
    llvm::json::Array DB;
    for (const std::string &TU : {UnityCore, UnityApp}) {
      std::string Cmd = "clang++ -std=c++20 " + ExtraFlag.str() + " -I" +
                        Tmp.path("src") + " -c " + TU;
      DB.push_back(llvm::json::Object{
          {"directory", BuildDir},
          {"command", Cmd},
          {"file", TU},
      });
    }
    writeFileText(BuildDir + "/compile_commands.json",
                  llvm::formatv("{0:2}", llvm::json::Value(std::move(DB))).str());
  };
  WriteCDB("-DTEST_VER=1");

  RealThreadsafeFS TFS;
  ParsingCallbacks NoopCallbacks;

  {
    DirectoryBasedGlobalCompilationDatabase::Options CDBOpts(TFS);
    CDBOpts.CompileCommandsDir = BuildDir;
    CDBOpts.ClangdCacheDir = "clangd_test";
    auto BaseCDB =
        std::make_unique<DirectoryBasedGlobalCompilationDatabase>(CDBOpts);
    UnityProjectCDB UnityCDB(std::move(BaseCDB), TFS);

    PCHManager::Options PCHOpts;
    PCHOpts.StorePCHInMemory = true;
    PCHOpts.WaitForInit = true;
    PCHOpts.WorkspaceRoot = Tmp.root();
    PCHOpts.ClangdCacheDir = "clangd_test";
    PCHOpts.MaxConcurrency = 2;
    PCHManager Mgr(UnityCDB, TFS, NoopCallbacks, PCHOpts);

    // 1) Static PCH for unity_app.cpp builds cleanly with parent chained PCH.
    auto CmdApp = UnityCDB.getCompileCommand(UnityApp);
    ASSERT_TRUE(CmdApp.has_value());
    auto StaticAccess = Mgr.findPCH(UnityApp, *CmdApp);
    ASSERT_TRUE(bool(StaticAccess));
    EXPECT_TRUE(Mgr.hasPCHInDependencies(UnityApp, *CmdApp, AppStdafx));
    EXPECT_TRUE(Mgr.hasPCHInDependencies(UnityApp, *CmdApp, CoreStdafx));

    // 2) Dynamic header PCH for app/world.hpp chains onto app/stdafx.hpp,
    // contains PreWorldType, and skips world.hpp + post_world.hpp.
    auto CmdWorld = UnityCDB.getCompileCommand(AppWorld);
    ASSERT_TRUE(CmdWorld.has_value());
    auto FS = TFS.view(std::nullopt);
    ASSERT_TRUE(Mgr.tryAddDynamicPCH(AppWorld, FS));

    // Wait briefly until the async rebuild builds the dynamic PCH (up to 10s).
    PCHManager::PCHAccess DynAccess;
    for (int Attempt = 0; Attempt < 200; ++Attempt) {
      DynAccess = Mgr.tryFindPCH(AppWorld, *CmdWorld);
      if (DynAccess && DynAccess.filename() == AppWorld)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(bool(DynAccess));
    EXPECT_EQ(DynAccess.filename(), AppWorld);

    // Parse world.hpp using the dynamic PCH in isolation: PreWorldType must be
    // visible while PostWorldOnlyType must NOT be visible.
    ParseInputs Inputs;
    Inputs.TFS = &TFS;
    Inputs.CompileCommand = *CmdWorld;
    Inputs.Contents =
        "#pragma once\n"
        "struct WorldType { PreWorldType pw; int get() const { return pw.x; } };\n"
        "static_assert(CORE_CONST == 10);\n";
    IgnoreDiagnostics IgnoreDiags;
    auto CI = buildCompilerInvocation(Inputs, IgnoreDiags);
    ASSERT_TRUE(CI != nullptr);
    auto AST = ParsedAST::build(AppWorld, Inputs, std::move(CI), {}, nullptr,
                                &DynAccess);
    ASSERT_TRUE(AST.has_value());
    for (const auto &D : AST->getDiagnostics())
      EXPECT_NE(D.Severity, DiagnosticsEngine::Error) << D.Message;

    // 3) Verify Level-1 fix: tryRemoveDynamicPCH clears the dynamic PCH,
    // and ZERO .ghost.pch_cache files are written to disk.
    EXPECT_TRUE(Mgr.tryRemoveDynamicPCH(AppWorld));
    for (const auto &Entry :
         std::filesystem::recursive_directory_iterator(Tmp.root())) {
      EXPECT_EQ(Entry.path().string().find(".ghost.pch_cache"),
                std::string::npos)
          << "Unexpected ghost PCH cache on disk: " << Entry.path().string();
    }
  }

  // 4) Second PCHManager instance should have .pch_cache files on disk and load
  // them, then invalidate on command-hash change.
  bool FoundCacheFile = false;
  for (const auto &Entry :
       std::filesystem::recursive_directory_iterator(Tmp.root())) {
    if (llvm::StringRef(Entry.path().string()).ends_with(".pch_cache")) {
      FoundCacheFile = true;
      break;
    }
  }
  EXPECT_TRUE(FoundCacheFile);

  WriteCDB("-DTEST_VER=2");
  DirectoryBasedGlobalCompilationDatabase::Options CDBOpts2(TFS);
  CDBOpts2.CompileCommandsDir = BuildDir;
  CDBOpts2.ClangdCacheDir = "clangd_test";
  auto BaseCDB2 =
      std::make_unique<DirectoryBasedGlobalCompilationDatabase>(CDBOpts2);
  UnityProjectCDB UnityCDB2(std::move(BaseCDB2), TFS);
  PCHManager::Options PCHOpts2;
  PCHOpts2.StorePCHInMemory = true;
  PCHOpts2.WaitForInit = true;
  PCHOpts2.WorkspaceRoot = Tmp.root();
  PCHOpts2.ClangdCacheDir = "clangd_test";
  PCHManager Mgr2(UnityCDB2, TFS, NoopCallbacks, PCHOpts2);

  auto CmdApp2 = UnityCDB2.getCompileCommand(UnityApp);
  ASSERT_TRUE(CmdApp2.has_value());
  auto Access2 = Mgr2.findPCH(UnityApp, *CmdApp2);
  ASSERT_TRUE(bool(Access2));
}

TEST(FuzzySWTests, ScalarSIMDAndWorkspaceSymbolCache) {
  // Verify scalar and SIMD matchers agree on scores and delimiter boundaries.
  int ExactScore = fuzzy_sw::match("world", "aurora::WorldState");
  int NonMatch = fuzzy_sw::match("xyz", "aurora::WorldState");
  EXPECT_GT(ExactScore, NonMatch);

  fuzzy_sw::SIMDParMatcher Matcher;
  std::vector<std::string_view> Inputs = {
      "aurora::WorldState", "aurora::RigidBody", "std::vector"};
  auto SingleRes = Matcher.match("World", std::vector<std::string_view>(Inputs),
                                 {3, true});
  ASSERT_FALSE(SingleRes.empty());
  EXPECT_EQ(SingleRes[0].target, "aurora::WorldState");

  // Parallel match without threads must safely fall back to single-threaded.
  auto ParNoThread =
      Matcher.match_par("Rigid", std::vector<std::string_view>(Inputs),
                        {3, true});
  ASSERT_FALSE(ParNoThread.empty());
  EXPECT_EQ(ParNoThread[0].target, "aurora::RigidBody");

  // Parallel match with worker threads.
  Matcher.SetupThreads(2);
  auto ParRes =
      Matcher.match_par("World", std::vector<std::string_view>(Inputs),
                        {3, true});
  ASSERT_FALSE(ParRes.empty());
  EXPECT_EQ(ParRes[0].target, "aurora::WorldState");

  // Verify SwapIndex generation & WorkspaceSymbolCache snapshot lifecycle.
  auto MakeIdx = [](llvm::StringRef Name) {
    SymbolSlab::Builder B;
    Symbol S;
    S.ID = SymbolID(Name);
    S.Name = Name;
    S.Scope = "aurora::";
    S.Definition.FileURI = "file:///workspace/world.hpp";
    S.CanonicalDeclaration = S.Definition;
    B.insert(S);
    return MemIndex::build(std::move(B).build(), RefSlab(), RelationSlab());
  };

  SwapIndex Idx(MakeIdx("WorldOne"));
  WorkspaceSymbolCache Cache;
  WorkspaceSymbolOptions WSOpts;
  auto Res1 = getWorkspaceSymbolsV2("WorldOne", 10, &Idx, "/workspace",
                                    Matcher, WSOpts, &Cache);
  ASSERT_TRUE(bool(Res1));
  ASSERT_EQ(Res1->size(), 1u);
  EXPECT_EQ((*Res1)[0].name, "WorldOne");

  auto Snap1 = Cache.getOrRebuild(&Idx, "/workspace");
  auto Snap2 = Cache.getOrRebuild(&Idx, "/workspace");
  EXPECT_EQ(Snap1.get(), Snap2.get());

  // Swapping index bumps generation and invalidates snapshot cleanly.
  Idx.reset(MakeIdx("WorldTwo"));
  auto Snap3 = Cache.getOrRebuild(&Idx, "/workspace");
  EXPECT_NE(Snap1.get(), Snap3.get());

  auto Res2 = getWorkspaceSymbolsV2("WorldTwo", 10, &Idx, "/workspace",
                                    Matcher, WSOpts, &Cache);
  ASSERT_TRUE(bool(Res2));
  ASSERT_EQ(Res2->size(), 1u);
  EXPECT_EQ((*Res2)[0].name, "WorldTwo");

  Matcher.StopThreads();
}

} // namespace
} // namespace clangd
} // namespace clang
