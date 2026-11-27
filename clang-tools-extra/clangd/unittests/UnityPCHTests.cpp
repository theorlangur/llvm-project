//===-- UnityPCHTests.cpp - UnityProjectCDB, PCHManager, and FuzzySW tests -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ClangdServer.h"
#include "CompileCommands.h"
#include "Compiler.h"
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
#include "support/Logger.h"
#include "support/Path.h"
#include "support/ThreadsafeFS.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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

TEST(UnityPCHTests, ProjectWithActiveIncludePCHFlags) {
  // Part A: CommandMangler strips pre-existing build-system PCH flags in both
  // GCC/Clang and clang-cl modes without losing forced-include or input args.
  {
    auto Mangler = CommandMangler::forTests();
    tooling::CompileCommand Cmd;
    Cmd.CommandLine = {
        "clang++", "-std=c++20", "-Winvalid-pch",
        "-Xclang", "-include-pch", "-Xclang", "/nonexistent/cmake_pch.hxx.pch",
        "-Xclang", "-include", "-Xclang", "/path/to/cmake_pch.hxx",
        "-include-pch", "/nonexistent/other.pch",
        "-include-pch=/nonexistent/joined.pch",
        "-Xclang", "-emit-pch", "-Xclang", "-building-pch-with-obj",
        "-c", "foo.cpp"};
    Mangler(Cmd, "foo.cpp");
    EXPECT_THAT(Cmd.CommandLine,
                testing::AllOf(
                    testing::Not(testing::Contains("-include-pch")),
                    testing::Not(testing::Contains("/nonexistent/cmake_pch.hxx.pch")),
                    testing::Not(testing::Contains("/nonexistent/other.pch")),
                    testing::Not(testing::Contains("-include-pch=/nonexistent/joined.pch")),
                    testing::Not(testing::Contains("-emit-pch")),
                    testing::Not(testing::Contains("-building-pch-with-obj")),
                    testing::Not(testing::Contains("-Winvalid-pch")),
                    testing::Contains("-include"),
                    testing::Contains("/path/to/cmake_pch.hxx")));
  }
  {
    auto Mangler = CommandMangler::forTests();
    tooling::CompileCommand Cmd;
    Cmd.CommandLine = {
        "clang-cl", "/Yustdafx.h", "/Fpbuild/app.pch", "/Fp:", "build/alt.pch",
        "/clang:-include-pch", "/clang:build/c.pch", "/c", "foo.cpp"};
    Mangler(Cmd, "foo.cpp");
    EXPECT_THAT(Cmd.CommandLine,
                testing::AllOf(
                    testing::Not(testing::Contains("/Yustdafx.h")),
                    testing::Not(testing::Contains("/Fpbuild/app.pch")),
                    testing::Not(testing::Contains("/Fp:")),
                    testing::Not(testing::Contains("build/alt.pch")),
                    testing::Not(testing::Contains("/clang:-include-pch")),
                    testing::Not(testing::Contains("/clang:build/c.pch")),
                    testing::Contains("foo.cpp")));

    // Bare /Yu with no joined header must NOT consume the positional input file.
    tooling::CompileCommand HdrStopCmd;
    HdrStopCmd.CommandLine = {"clang-cl", "/Yu", "/Fpfoo.pch", "foo.cpp"};
    Mangler(HdrStopCmd, "foo.cpp");
    EXPECT_THAT(HdrStopCmd.CommandLine,
                testing::AllOf(testing::Not(testing::Contains("/Yu")),
                               testing::Not(testing::Contains("/Fpfoo.pch")),
                               testing::Contains("foo.cpp")));
  }

  // Part B: End-to-end CMake CMAKE_UNITY_BUILD + target_precompile_headers
  // with 2 levels, cmake_pch.hxx wrappers, nonexistent .pch binaries on disk,
  // and a standalone -include-pch non-unity TU.
  TempDir Tmp("cmake-pch-unity-test");
  std::string BuildDir = Tmp.path("build");

  std::string CoreStdafx = Tmp.path("src/core/stdafx.hpp");
  std::string CoreImpl = Tmp.path("src/core/impl.cpp");
  std::string AppStdafx = Tmp.path("src/app/stdafx.hpp");
  std::string AppPre = Tmp.path("src/app/pre.hpp");
  std::string AppWorld = Tmp.path("src/app/world.hpp");
  std::string AppPost = Tmp.path("src/app/post.hpp");
  std::string AppImpl = Tmp.path("src/app/impl.cpp");

  std::string CoreCMakePCH = BuildDir + "/CMakeFiles/core.dir/cmake_pch.hxx";
  std::string CoreCMakePCHSrc =
      BuildDir + "/CMakeFiles/core.dir/cmake_pch.hxx.cxx";
  std::string CoreCMakePCHBin =
      BuildDir + "/CMakeFiles/core.dir/cmake_pch.hxx.pch";
  std::string CoreUnity =
      BuildDir + "/CMakeFiles/core.dir/Unity/unity_0_cxx.cxx";

  std::string AppCMakePCH = BuildDir + "/CMakeFiles/app.dir/cmake_pch.hxx";
  std::string AppCMakePCHSrc =
      BuildDir + "/CMakeFiles/app.dir/cmake_pch.hxx.cxx";
  std::string AppCMakePCHBin =
      BuildDir + "/CMakeFiles/app.dir/cmake_pch.hxx.pch";
  std::string AppUnity =
      BuildDir + "/CMakeFiles/app.dir/Unity/unity_0_cxx.cxx";

  std::string ToolPCHHdr = Tmp.path("src/tools/tool_pch.hpp");
  std::string ToolPCHBin = BuildDir + "/tool_pch.hpp.pch";
  std::string ToolCpp = Tmp.path("src/tools/tool.cpp");

  writeFileText(CoreStdafx,
                "#pragma once\n#define CORE_MAGIC 42\ninline int core_val() { return CORE_MAGIC; }\n");
  writeFileText(CoreImpl, "int call_core() { return core_val(); }\n");
  writeFileText(AppPre, "#pragma once\nstruct PreWorld { int a = core_val(); };\n");
  writeFileText(AppWorld,
                "#pragma once\nstruct WorldThing { PreWorld p; };\n");
  writeFileText(AppPost, "#pragma once\nstruct PostWorldOnly { int z = 9; };\n");
  writeFileText(AppStdafx,
                "#pragma once\n#include \"core/stdafx.hpp\"\n"
                "#include \"app/pre.hpp\"\n#include \"app/world.hpp\"\n"
                "#include \"app/post.hpp\"\n");
  writeFileText(AppImpl, "int call_app() { WorldThing w; return w.p.a; }\n");

  writeFileText(CoreCMakePCH, "#include \"" + CoreStdafx + "\"\n");
  writeFileText(CoreCMakePCHSrc, "/* CMake PCH TU */\n");
  writeFileText(CoreUnity, "#include \"" + CoreImpl + "\"\n");

  writeFileText(AppCMakePCH, "#include \"" + AppStdafx + "\"\n");
  writeFileText(AppCMakePCHSrc, "/* CMake PCH TU */\n");
  writeFileText(AppUnity, "#include \"" + AppImpl + "\"\n");

  writeFileText(ToolPCHHdr, "#pragma once\ninline int tool_helper() { return 7; }\n");
  writeFileText(ToolCpp, "int use_tool() { return tool_helper(); }\n");
  // Intentionally write a corrupt external .pch to ensure clangd never loads it.
  writeFileText(CoreCMakePCHBin, "NOT_A_VALID_PCH");

  llvm::json::Array DB;
  DB.push_back(llvm::json::Object{
      {"directory", BuildDir},
      {"command", "clang++ -std=c++20 -I" + Tmp.path("src") +
                      " -Winvalid-pch -Xclang -emit-pch -Xclang -include "
                      "-Xclang " + CoreCMakePCH + " -c " + CoreCMakePCHSrc},
      {"file", CoreCMakePCHSrc},
  });
  DB.push_back(llvm::json::Object{
      {"directory", BuildDir},
      {"command", "clang++ -std=c++20 -I" + Tmp.path("src") +
                      " -Winvalid-pch -Xclang -include-pch -Xclang " +
                      CoreCMakePCHBin + " -Xclang -include -Xclang " +
                      CoreCMakePCH + " -c " + CoreUnity},
      {"file", CoreUnity},
  });
  DB.push_back(llvm::json::Object{
      {"directory", BuildDir},
      {"command", "clang++ -std=c++20 -I" + Tmp.path("src") +
                      " -Winvalid-pch -Xclang -emit-pch -Xclang -include "
                      "-Xclang " + AppCMakePCH + " -c " + AppCMakePCHSrc},
      {"file", AppCMakePCHSrc},
  });
  DB.push_back(llvm::json::Object{
      {"directory", BuildDir},
      {"command", "clang++ -std=c++20 -I" + Tmp.path("src") +
                      " -Winvalid-pch -Xclang -include-pch -Xclang " +
                      AppCMakePCHBin + " -Xclang -include -Xclang " +
                      AppCMakePCH + " -c " + AppUnity},
      {"file", AppUnity},
  });
  DB.push_back(llvm::json::Object{
      {"directory", BuildDir},
      {"command", "clang++ -std=c++20 -I" + Tmp.path("src") +
                      " -include-pch " + ToolPCHBin + " -c " + ToolCpp},
      {"file", ToolCpp},
  });
  writeFileText(BuildDir + "/compile_commands.json",
                llvm::formatv("{0:2}", llvm::json::Value(std::move(DB))).str());

  RealThreadsafeFS TFS;
  DirectoryBasedGlobalCompilationDatabase::Options CDBOpts(TFS);
  CDBOpts.CompileCommandsDir = BuildDir;
  CDBOpts.ClangdCacheDir = "clangd_test";
  auto BaseCDB =
      std::make_unique<DirectoryBasedGlobalCompilationDatabase>(CDBOpts);
  auto UnityOwner = std::make_unique<UnityProjectCDB>(std::move(BaseCDB), TFS);
  OverlayCDB MangledCDB(UnityOwner.get(), {}, CommandMangler::forTests());

  auto WorldSpec = MangledCDB.getPCHSpec(AppWorld);
  ASSERT_TRUE(WorldSpec.has_value());
  EXPECT_TRUE(WorldSpec->IsDynamic);
  EXPECT_EQ(llvm::sys::path::filename(WorldSpec->UmbrellaHeader), "stdafx.hpp");
  EXPECT_EQ(llvm::sys::path::filename(WorldSpec->ParentPCHHeader), "stdafx.hpp");
  // Ensure BaseCmd stripped cmake_pch.hxx so HdrCmd and DynSpec stay isolated.
  auto WorldCmd = MangledCDB.getCompileCommand(AppWorld);
  ASSERT_TRUE(WorldCmd.has_value());
  EXPECT_THAT(WorldCmd->CommandLine,
              testing::AllOf(testing::Not(testing::Contains(AppCMakePCH)),
                             testing::Not(testing::Contains(AppCMakePCHBin)),
                             testing::Not(testing::Contains("-include-pch"))));

  // Standalone -include-pch without -include recovers -include tool_pch.hpp.
  auto ToolCmd = MangledCDB.getCompileCommand(ToolCpp);
  ASSERT_TRUE(ToolCmd.has_value());
  EXPECT_THAT(ToolCmd->CommandLine,
              testing::AllOf(testing::Not(testing::Contains("-include-pch")),
                             testing::Contains("-include"),
                             testing::Contains(ToolPCHHdr)));

  ParsingCallbacks NoopCallbacks;
  PCHManager::Options PCHOpts;
  PCHOpts.StorePCHInMemory = true;
  PCHOpts.WaitForInit = true;
  PCHOpts.WorkspaceRoot = Tmp.root();
  PCHOpts.ClangdCacheDir = "clangd_test";
  PCHOpts.MaxConcurrency = 2;
  PCHManager Mgr(MangledCDB, TFS, NoopCallbacks, PCHOpts);

  auto FS = TFS.view(std::nullopt);
  ASSERT_TRUE(Mgr.tryAddDynamicPCH(AppWorld, FS));
  PCHManager::PCHAccess DynAccess;
  for (int Attempt = 0; Attempt < 200; ++Attempt) {
    DynAccess = Mgr.tryFindPCH(AppWorld, *WorldCmd);
    if (DynAccess && DynAccess.filename() == AppWorld)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ASSERT_TRUE(bool(DynAccess));

  // Verify WorldThing compiles cleanly in isolation (no redefinition from
  // leaked cmake_pch.hxx; PreWorld is visible; PostWorldOnly is not).
  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.CompileCommand = *WorldCmd;
  Inputs.Contents =
      "#pragma once\n"
      "struct WorldThing { PreWorld p; int ok = CORE_MAGIC; };\n";
  IgnoreDiagnostics IgnoreDiags;
  auto CI = buildCompilerInvocation(Inputs, IgnoreDiags);
  ASSERT_TRUE(CI != nullptr);
  auto AST = ParsedAST::build(AppWorld, Inputs, std::move(CI), {}, nullptr,
                              &DynAccess);
  ASSERT_TRUE(AST.has_value());
  for (const auto &D : AST->getDiagnostics())
    EXPECT_NE(D.Severity, DiagnosticsEngine::Error) << D.Message;

  // Verify standalone tool.cpp with recovered -include builds cleanly too.
  ParseInputs ToolInputs;
  ToolInputs.TFS = &TFS;
  ToolInputs.CompileCommand = *ToolCmd;
  ToolInputs.Contents = "int use_tool() { return tool_helper(); }\n";
  auto ToolCI = buildCompilerInvocation(ToolInputs, IgnoreDiags);
  ASSERT_TRUE(ToolCI != nullptr);
  auto ToolPreamble = buildPreamble(ToolCpp, *ToolCI, ToolInputs, true, {});
  ASSERT_TRUE(ToolPreamble != nullptr);
  auto ToolAST = ParsedAST::build(ToolCpp, ToolInputs, std::move(ToolCI), {},
                                  ToolPreamble, nullptr);
  ASSERT_TRUE(ToolAST.has_value());
  for (const auto &D : ToolAST->getDiagnostics())
    EXPECT_NE(D.Severity, DiagnosticsEngine::Error) << D.Message;
}

TEST(UnityPCHTests, LowerLayerHeaderEditInvalidationAndSafety) {
  TempDir Tmp("lower-layer-edit-test");
  std::string BuildDir = Tmp.path("build");

  std::string CoreStdafx = Tmp.path("src/core/stdafx.hpp");
  std::string CoreVec3 = Tmp.path("src/core/vec3.hpp");
  std::string CoreImpl = Tmp.path("src/core/impl.cpp");
  std::string AppStdafx = Tmp.path("src/app/stdafx.hpp");
  std::string AppWorld = Tmp.path("src/app/world.hpp");
  std::string AppImpl = Tmp.path("src/app/impl.cpp");
  std::string UnityCore = Tmp.path("src/unity_core.cpp");
  std::string UnityApp = Tmp.path("src/unity_app.cpp");

  std::string Vec3V1 = "#pragma once\nstruct Vec3 { int x = 1; };\n";
  std::string WorldV1 =
      "#pragma once\nstruct World { Vec3 v; int val() const { return v.x; } };\n";

  writeFileText(CoreVec3, Vec3V1);
  writeFileText(CoreStdafx, "#pragma once\n#include \"core/vec3.hpp\"\n");
  writeFileText(CoreImpl, "int f_core() { return Vec3{}.x; }\n");
  writeFileText(AppWorld, WorldV1);
  writeFileText(
      AppStdafx,
      "#pragma once\n#include \"core/stdafx.hpp\"\n#include \"app/world.hpp\"\n");
  writeFileText(AppImpl, "int f_app() { return World{}.val(); }\n");
  writeFileText(UnityCore,
                "#include \"core/stdafx.hpp\"\n#include \"core/impl.cpp\"\n");
  writeFileText(UnityApp,
                "#include \"app/stdafx.hpp\"\n#include \"app/impl.cpp\"\n");

  llvm::json::Array DB;
  for (const std::string &TU : {UnityCore, UnityApp}) {
    std::string Cmd =
        "clang++ -std=c++20 -I" + Tmp.path("src") + " -c " + TU;
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
  CDBOpts.ClangdCacheDir = "clangd_test";
  auto BaseCDB =
      std::make_unique<DirectoryBasedGlobalCompilationDatabase>(CDBOpts);
  auto UnityOwner = std::make_unique<UnityProjectCDB>(std::move(BaseCDB), TFS);
  OverlayCDB MangledCDB(UnityOwner.get(), {}, CommandMangler::forTests());

  struct DiagCapture : public ClangdServer::Callbacks {
    std::mutex Mu;
    llvm::StringMap<std::vector<Diag>> DiagsByFile;
    void onDiagnosticsReady(PathRef File, llvm::StringRef,
                            llvm::ArrayRef<Diag> Diagnostics) override {
      std::lock_guard<std::mutex> G(Mu);
      DiagsByFile[File].assign(Diagnostics.begin(), Diagnostics.end());
    }
  } Capture;

  ClangdServer::Options SOpts = ClangdServer::optsForTest();
  SOpts.EnablePCH = true;
  SOpts.StorePCHInMemory = true;
  SOpts.PCHAlwaysWait = true;
  SOpts.WorkspaceRoot = Tmp.root();
  SOpts.ClangdCacheDir = "clangd_test";
  SOpts.PCHMaxConcurrency = 2;

  ClangdServer Server(MangledCDB, TFS, SOpts, &Capture);
  Server.addDocument(CoreVec3, Vec3V1, "1", WantDiagnostics::Yes);
  Server.addDocument(AppWorld, WorldV1, "1", WantDiagnostics::Yes);
  ASSERT_TRUE(Server.blockUntilIdleForTest(30));

  // Unsaved draft edits changing size heavily must not crash or corrupt L2.
  std::string Vec3Draft =
      "#pragma once\n// extra draft padding to alter file byte size...\n"
      "struct Vec3 { int x = 1; int tmp_unsaved(); };\n";
  Server.addDocument(CoreVec3, Vec3Draft, "2", WantDiagnostics::Yes, false,
                     /*AfterChange=*/true);
  ASSERT_TRUE(Server.blockUntilIdleForTest(30));

  // Now save lower-layer header with a new symbol and reference it from L2.
  std::string Vec3V2 =
      "#pragma once\n// Saved lower-layer header with new API\n"
      "struct Vec3 { int x = 1; };\n"
      "inline int newlyAddedLowerLayerFn() { return 123; }\n";
  std::string WorldV2 =
      "#pragma once\nstruct World { Vec3 v; int val() const { return v.x + newlyAddedLowerLayerFn(); } };\n";
  writeFileText(CoreVec3, Vec3V2);
  writeFileText(AppWorld, WorldV2);
  Server.addDocument(CoreVec3, Vec3V2, "3", WantDiagnostics::Yes);
  EXPECT_TRUE(Server.checkChangedFile(CoreVec3));
  Server.addDocument(AppWorld, WorldV2, "2", WantDiagnostics::Yes);
  ASSERT_TRUE(Server.blockUntilIdleForTest(30));

  {
    std::lock_guard<std::mutex> G(Capture.Mu);
    for (const auto &F : {CoreVec3, AppWorld}) {
      auto It = Capture.DiagsByFile.find(F);
      ASSERT_NE(It, Capture.DiagsByFile.end()) << "Missing diags for " << F;
      for (const auto &D : It->second)
        EXPECT_NE(D.Severity, DiagnosticsEngine::Error) << F << ": " << D.Message;
    }
  }

  // Directly verify PCHAccess::addPCH size-mismatch safety guard: if a workspace
  // header changes size on disk before invalidation runs, addPCH rejects the
  // stale PCH snapshot rather than letting Clang read invalid SLocEntry offsets.
  ParsingCallbacks NoopCallbacks;
  PCHManager::Options PCHOpts;
  PCHOpts.StorePCHInMemory = true;
  PCHOpts.WaitForInit = true;
  PCHOpts.WorkspaceRoot = Tmp.root();
  PCHOpts.ClangdCacheDir = "clangd_test";
  PCHManager GuardMgr(MangledCDB, TFS, NoopCallbacks, PCHOpts);
  auto CmdApp = MangledCDB.getCompileCommand(AppImpl);
  ASSERT_TRUE(CmdApp.has_value());
  auto Access = GuardMgr.findPCH(AppImpl, *CmdApp);
  ASSERT_TRUE(bool(Access));

  writeFileText(CoreVec3, Vec3V2 + "\n// Resize before invalidation runs\nint extra_fn();\n");
  ParseInputs TestInputs;
  TestInputs.TFS = &TFS;
  TestInputs.CompileCommand = *CmdApp;
  IgnoreDiagnostics IgnoreDiags;
  auto TestCI = buildCompilerInvocation(TestInputs, IgnoreDiags);
  ASSERT_TRUE(TestCI != nullptr);
  auto FreshVFS = TFS.view(CmdApp->Directory);
  EXPECT_FALSE(Access.addPCH(TestCI.get(), FreshVFS));
  EXPECT_TRUE(GuardMgr.blockUntilIdle(timeoutSeconds(30)));
}

TEST(UnityPCHTests, ConfigLoadingValidationAndOutOfSourceBuild) {
  struct CapturingLogger : public Logger {
    std::mutex Mu;
    std::string Output;
    void log(Level L, const char *,
             const llvm::formatv_object_base &Message) override {
      std::lock_guard<std::mutex> G(Mu);
      Output += indicator(L);
      Output += ": ";
      Output += Message.str();
      Output += '\n';
    }
  } CapLogger;
  LoggingSession LogGuard(CapLogger);

  TempDir Tmp("unity-config-val-test");
  std::string BuildDir = Tmp.path("build");
  std::string CoreStdafx = Tmp.path("src/core/stdafx.hpp");
  std::string SharedHdr = Tmp.path("src/shared/common.hpp");
  std::string CoreImpl = Tmp.path("src/core/impl.cpp");
  std::string UnityCore = Tmp.path("src/unity_core.cpp");

  writeFileText(SharedHdr, "#pragma once\ninline int shared_val() { return 5; }\n");
  writeFileText(CoreStdafx,
                "#pragma once\n#include \"shared/common.hpp\"\ninline int core_val() { return shared_val(); }\n");
  writeFileText(CoreImpl, "int call_core() { return core_val(); }\n");
  writeFileText(UnityCore,
                "#include \"core/stdafx.hpp\"\n#include \"core/impl.cpp\"\n");

  // Out-of-source compile_commands.json in build/, while unity_pch.json is in
  // the parent workspace root.
  llvm::json::Array DB;
  DB.push_back(llvm::json::Object{
      {"directory", BuildDir},
      {"command", "clang++ -std=c++20 -I" + Tmp.path("src") + " -c " + UnityCore},
      {"file", UnityCore},
  });
  writeFileText(BuildDir + "/compile_commands.json",
                llvm::formatv("{0:2}", llvm::json::Value(std::move(DB))).str());

  std::string MissingUmbrella = Tmp.path("src/missing/stdafx.hpp");
  std::string MissingFilterDir = Tmp.path("nonexistent_dir");
  writeFileText(Tmp.path("unity_pch.json"), R"json({
  "unknown-key": 123,
  "filter-in": ["src/", "nonexistent_dir/"],
  "filter-out": [""],
  "skip-deps": ["[unclosed"],
  "pch": [
    {
      "file": "src/core/stdafx.hpp",
      "allow-includes-from": ["src/shared"]
    },
    {
      "file": "src/missing/stdafx.hpp",
      "allow-includes-from": ["src/missing_inc"]
    }
  ]
})json");

  RealThreadsafeFS TFS;
  DirectoryBasedGlobalCompilationDatabase::Options CDBOpts(TFS);
  CDBOpts.CompileCommandsDir = BuildDir;
  auto BaseCDB =
      std::make_unique<DirectoryBasedGlobalCompilationDatabase>(CDBOpts);
  UnityProjectCDB UnityCDB(std::move(BaseCDB), TFS);

  // Trigger discovery and verify valid config entries work (shared/common.hpp
  // is allowed via pch[0].allow-includes-from and empty "" in filter-out
  // was ignored rather than excluding the whole repo).
  auto SharedSpec = UnityCDB.getPCHSpec(SharedHdr);
  ASSERT_TRUE(SharedSpec.has_value());
  EXPECT_TRUE(SharedSpec->IsDynamic);
  EXPECT_EQ(SharedSpec->UmbrellaHeader, CoreStdafx);

  std::string Logs;
  {
    std::lock_guard<std::mutex> G(CapLogger.Mu);
    Logs = CapLogger.Output;
  }
  EXPECT_THAT(Logs, testing::HasSubstr("Config: discovered"));
  EXPECT_THAT(Logs, testing::HasSubstr("unknown top-level key 'unknown-key'"));
  EXPECT_THAT(Logs, testing::HasSubstr("filter-in[1]"));
  EXPECT_THAT(Logs, testing::HasSubstr("does not exist"));
  EXPECT_THAT(Logs, testing::HasSubstr("'filter-out[0]' is empty"));
  EXPECT_THAT(Logs, testing::HasSubstr("invalid regex in 'skip-deps[0]'"));
  EXPECT_THAT(Logs, testing::HasSubstr("did not match any discovered unity umbrella header"));
  EXPECT_THAT(Logs, testing::HasSubstr("Config filter summary: 1/1 C++ TU(s) passed filters"));
}

} // namespace
} // namespace clangd
} // namespace clang
