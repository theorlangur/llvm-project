//===--- UnityProjectCDB.cpp - Auto-discover unity/PCH hierarchies -*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UnityProjectCDB.h"
#include "support/Logger.h"
#include "clang/Lex/DependencyDirectivesScanner.h"
#include "clang/Basic/TokenKinds.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"
#include <algorithm>
#include <optional>
#include <utility>

namespace clang {
namespace clangd {
namespace {

std::string normalizePath(PathRef Path) {
  llvm::SmallString<256> Buf(Path);
  llvm::sys::path::native(Buf);
  llvm::sys::path::remove_dots(Buf, /*remove_dot_dot=*/true);
  return normalizePathDriveLetter(Buf);
}

std::string resolveRelativeTo(PathRef BaseDir, PathRef RelOrAbs) {
  if (llvm::sys::path::is_absolute(RelOrAbs))
    return normalizePath(RelOrAbs);
  llvm::SmallString<256> Buf(BaseDir);
  llvm::sys::path::append(Buf, RelOrAbs);
  return normalizePath(Buf);
}

bool isCppImplExtension(PathRef File) {
  StringRef Ext = llvm::sys::path::extension(File);
  return Ext.equals_insensitive(".cpp") || Ext.equals_insensitive(".cc") ||
         Ext.equals_insensitive(".cxx") || Ext.equals_insensitive(".c++") ||
         Ext.equals_insensitive(".cp");
}

bool isHeaderExtension(PathRef File) {
  StringRef Ext = llvm::sys::path::extension(File);
  return Ext.equals_insensitive(".h") || Ext.equals_insensitive(".hpp") ||
         Ext.equals_insensitive(".hh") || Ext.equals_insensitive(".hxx") ||
         Ext.equals_insensitive(".h++") || Ext.equals_insensitive(".inl");
}

bool isClangCLMode(const tooling::CompileCommand &Cmd) {
  if (Cmd.CommandLine.empty())
    return false;
  StringRef Driver = llvm::sys::path::stem(Cmd.CommandLine.front());
  if (Driver.equals_insensitive("clang-cl") || Driver.equals_insensitive("cl"))
    return true;
  for (const std::string &Arg : Cmd.CommandLine) {
    if (Arg == "--driver-mode=cl")
      return true;
  }
  return false;
}

std::vector<std::string>
extractIncludeDirs(const tooling::CompileCommand &Cmd) {
  std::vector<std::string> Dirs;
  for (size_t I = 0; I < Cmd.CommandLine.size(); ++I) {
    StringRef Arg = Cmd.CommandLine[I];
    if ((Arg == "-I" || Arg == "/I" || Arg == "-isystem") &&
        I + 1 < Cmd.CommandLine.size()) {
      Dirs.push_back(resolveRelativeTo(Cmd.Directory, Cmd.CommandLine[++I]));
    } else if (Arg.starts_with("-I") && Arg.size() > 2) {
      Dirs.push_back(resolveRelativeTo(Cmd.Directory, Arg.drop_front(2)));
    } else if (Arg.starts_with("/I") && Arg.size() > 2) {
      Dirs.push_back(resolveRelativeTo(Cmd.Directory, Arg.drop_front(2)));
    } else if (Arg.starts_with("-isystem") && Arg.size() > 8) {
      Dirs.push_back(resolveRelativeTo(Cmd.Directory, Arg.drop_front(8)));
    }
  }
  return Dirs;
}

// Strips output/compile-target/input-file arguments from a unity compile command
// so it can serve as a clean base command for headers, PCHs, and .cpp files.
tooling::CompileCommand
stripTargetAndOutputArgs(const tooling::CompileCommand &Orig,
                         PathRef OrigFileNorm, bool ClangCL) {
  tooling::CompileCommand Out = Orig;
  std::vector<std::string> Clean;
  Clean.reserve(Orig.CommandLine.size());

  StringRef OrigFilename = llvm::sys::path::filename(OrigFileNorm);
  bool AfterDashDash = false;

  for (size_t I = 0; I < Orig.CommandLine.size(); ++I) {
    const std::string &Arg = Orig.CommandLine[I];
    StringRef S(Arg);

    if (I == 0) {
      Clean.push_back(Arg);
      continue;
    }

    if (S == "--") {
      AfterDashDash = true;
      continue;
    }
    if (AfterDashDash)
      continue;

    if (S == "-o" || S == "-MF" || S == "-MT" || S == "-MQ" || S == "-MJ") {
      ++I; // Skip flag and its value.
      continue;
    }
    if (S.starts_with("-o") && S.size() > 2)
      continue;
    if (S == "-MD" || S == "-MMD" || S == "-c" || S == "/c")
      continue;
    if (S == "-x" && I + 1 < Orig.CommandLine.size()) {
      ++I;
      continue;
    }
    if (S.starts_with("-x"))
      continue;

    if (ClangCL &&
        (S.starts_with("/Fo") || S.starts_with("/Fd") || S.starts_with("/Fp") ||
         S.starts_with("/Yc") || S.starts_with("/Yu") || S == "/bigobj" ||
         S.starts_with("/Tc") || S.starts_with("/Tp")))
      continue;

    // Skip the original unity source file argument.
    if (llvm::sys::path::filename(S) == OrigFilename) {
      std::string Resolved = resolveRelativeTo(Orig.Directory, S);
      if (pathEqual(Resolved, OrigFileNorm))
        continue;
    }

    Clean.push_back(Arg);
  }
  Out.CommandLine = std::move(Clean);
  Out.Output.clear();
  return Out;
}

void appendIncludeArg(std::vector<std::string> &CmdLine, PathRef Header,
                      bool ClangCL) {
  if (ClangCL) {
    CmdLine.push_back("/clang:-include");
    CmdLine.push_back(("/clang:" + Header).str());
  } else {
    CmdLine.push_back("-include");
    CmdLine.push_back(Header.str());
  }
}

void appendHeaderTypeAndTarget(std::vector<std::string> &CmdLine, PathRef Target,
                               bool ClangCL) {
  if (ClangCL) {
    CmdLine.push_back("/clang:-x");
    CmdLine.push_back("/clang:c++-header");
    CmdLine.push_back("/c");
    CmdLine.push_back(Target.str());
  } else {
    CmdLine.push_back("-x");
    CmdLine.push_back("c++-header");
    CmdLine.push_back("-c");
    CmdLine.push_back(Target.str());
  }
}

void appendCppTarget(std::vector<std::string> &CmdLine, PathRef Target,
                     bool ClangCL) {
  CmdLine.push_back(ClangCL ? "/c" : "-c");
  CmdLine.push_back(Target.str());
}

// Scans \p File using Clang's DependencyDirectivesScanner and returns resolved
// quoted #include paths in source order.
std::vector<std::string>
scanQuotedIncludes(PathRef File, llvm::vfs::FileSystem &VFS,
                   llvm::ArrayRef<std::string> ExtraIncludeDirs) {
  std::vector<std::string> Result;
  auto Buf = VFS.getBufferForFile(File);
  if (!Buf)
    return Result;

  StringRef Text = (*Buf)->getBuffer();
  SmallVector<dependency_directives_scan::Token, 64> Tokens;
  SmallVector<dependency_directives_scan::Directive, 16> Directives;
  if (scanSourceForDependencyDirectives(Text, Tokens, Directives))
    return Result;

  StringRef FileDir = llvm::sys::path::parent_path(File);
  for (const auto &Dir : Directives) {
    if (Dir.Kind != dependency_directives_scan::pp_include &&
        Dir.Kind != dependency_directives_scan::pp_import)
      continue;
    for (const auto &Tok : Dir.Tokens) {
      if (Tok.is(tok::header_name)) {
        StringRef Raw = Text.substr(Tok.Offset, Tok.Length);
        if (Raw.size() >= 2 && Raw.front() == '"' && Raw.back() == '"') {
          StringRef IncName = Raw.drop_front().drop_back();
          std::string Candidate = resolveRelativeTo(FileDir, IncName);
          if (VFS.exists(Candidate)) {
            Result.push_back(std::move(Candidate));
          } else {
            for (const std::string &IncDir : ExtraIncludeDirs) {
              std::string Alt = resolveRelativeTo(IncDir, IncName);
              if (VFS.exists(Alt)) {
                Result.push_back(std::move(Alt));
                break;
              }
            }
          }
        }
        break;
      }
    }
  }
  return Result;
}

bool isInAnyAllowedDir(PathRef File, llvm::ArrayRef<std::string> Dirs) {
  for (const std::string &D : Dirs) {
    if (pathStartsWith(D, File))
      return true;
  }
  return false;
}

} // namespace

UnityProjectCDB::UnityProjectCDB(
    const GlobalCompilationDatabase *Base, const ThreadsafeFS &TFS,
    Options Opts, std::optional<std::string> FallbackWorkingDirectory)
    : GlobalCompilationDatabase(FallbackWorkingDirectory), Base(Base),
      TFS(TFS), Opts(std::move(Opts)) {
  if (Base) {
    BaseChanged = Base->watch([this](const std::vector<std::string> &Changes) {
      onBaseCommandsChanged(Changes);
    });
  }
}

UnityProjectCDB::UnityProjectCDB(
    std::unique_ptr<GlobalCompilationDatabase> Base, const ThreadsafeFS &TFS,
    Options Opts, std::optional<std::string> FallbackWorkingDirectory)
    : UnityProjectCDB(Base.get(), TFS, std::move(Opts),
                      std::move(FallbackWorkingDirectory)) {
  BaseOwner = std::move(Base);
}

UnityProjectCDB::ConfigOverrides
UnityProjectCDB::loadConfig(PathRef SourceRoot) const {
  ConfigOverrides Out;
  if (SourceRoot.empty() && !Opts.ConfigPath)
    return Out;

  auto FS = TFS.view(SourceRoot);
  std::string PathToRead;
  if (Opts.ConfigPath && !Opts.ConfigPath->empty()) {
    PathToRead = *Opts.ConfigPath;
  } else {
    for (StringRef Candidate : {"unity_pch.json", ".clangd-unity.json"}) {
      std::string P = resolveRelativeTo(SourceRoot, Candidate);
      if (FS->exists(P)) {
        PathToRead = std::move(P);
        break;
      }
    }
  }
  if (PathToRead.empty())
  {
    log("(UnityCDB) no config found.");
    return Out;
  }

  log("(UnityCDB) config. Attempting to load from {0}", PathToRead);
  auto Buf = FS->getBufferForFile(PathToRead);
  if (!Buf)
  {
    elog("(UnityCDB) config: could not read");
    return Out;
  }

  auto Parsed = llvm::json::parse((*Buf)->getBuffer());
  if (!Parsed) {
    elog("(UnityCDB) Failed to parse config {0}: {1}", PathToRead,
         Parsed.takeError());
    return Out;
  }
  const auto *Obj = Parsed->getAsObject();
  if (!Obj)
  {
    elog("(UnityCDB) config: not root object found");
    return Out;
  }

  Out.Loaded = true;
  StringRef ConfigDir = llvm::sys::path::parent_path(PathToRead);
  StringRef BaseDir = SourceRoot.empty() ? ConfigDir : SourceRoot;

  auto ReadPathArray = [&](StringRef Key, std::vector<std::string> &Dest) {
    if (const auto *Arr = Obj->getArray(Key)) {
      for (const auto &V : *Arr) {
        if (auto S = V.getAsString())
          Dest.push_back(resolveRelativeTo(BaseDir, *S));
      }
    }
  };
  ReadPathArray("filter-in", Out.FilterIn);
  ReadPathArray("filter-out", Out.FilterOut);
  if (const auto *Arr = Obj->getArray("skip-deps")) {
    for (const auto &V : *Arr) {
      if (auto S = V.getAsString())
        Out.SkipDeps.push_back(S->str());
    }
  }
  if (const auto *Arr = Obj->getArray("pch")) {
    for (const auto &V : *Arr) {
      if (const auto *PObj = V.getAsObject()) {
        if (auto FileStr = PObj->getString("file")) {
          std::string Key = resolveRelativeTo(BaseDir, *FileStr);
          if (const auto *AllowArr = PObj->getArray("allow-includes-from")) {
            auto &Vec = Out.AllowIncludesFrom[Key];
            for (const auto &AV : *AllowArr) {
              if (auto AS = AV.getAsString())
                Vec.push_back(resolveRelativeTo(BaseDir, *AS));
            }
          }
        }
      }
    }
  }
  return Out;
}

void UnityProjectCDB::onBaseCommandsChanged(
    const std::vector<std::string> &Files) {
  scanFiles(Files);
  OnCommandChanged.broadcast(Files);
}

void UnityProjectCDB::scanFiles(llvm::ArrayRef<std::string> Files) {
  if (!Opts.Enabled || !Base || Files.empty())
    return;

  std::string SourceRoot;
  if (auto PI = Base->getProjectInfo(Files.front()))
    SourceRoot = PI->SourceRoot;
  ConfigOverrides Cfg = loadConfig(SourceRoot);

  auto ShouldProcess = [&](PathRef NormPath) -> bool {
    if (!Cfg.FilterOut.empty() && isInAnyAllowedDir(NormPath, Cfg.FilterOut))
      return false;
    if (!Cfg.FilterIn.empty() && !isInAnyAllowedDir(NormPath, Cfg.FilterIn))
      return false;
    return true;
  };

  struct UnityEntry {
    std::string UnityPath;
    std::string OwnerStdafx;
    tooling::CompileCommand BaseCmd;
    bool ClangCL = false;
    std::vector<std::string> ExtraIncDirs;
    std::vector<std::string> IncludedCpps;
  };

  std::vector<UnityEntry> UnityEntries;
  llvm::StringMap<UnityEntry *> StdafxToRepresentativeUnity;
  llvm::StringSet<> AllStdafxSet;

  // Phase 1: Identify unity files among Files.
  for (const std::string &RawFile : Files) {
    if (!isCppImplExtension(RawFile))
      continue;
    std::string NormFile = normalizePath(RawFile);
    if (!ShouldProcess(NormFile))
      continue;

    auto Cmd = Base->getCompileCommand(RawFile);
    if (!Cmd)
      continue;

    auto VFS = TFS.view(Cmd->Directory);
    std::vector<std::string> IncDirs = extractIncludeDirs(*Cmd);
    std::vector<std::string> Includes =
        scanQuotedIncludes(NormFile, *VFS, IncDirs);
    if (Includes.size() < 2)
      continue;

    if (!isHeaderExtension(Includes.front()))
      continue;

    std::vector<std::string> CppIncludes;
    for (size_t I = 1; I < Includes.size(); ++I) {
      if (isCppImplExtension(Includes[I]))
        CppIncludes.push_back(Includes[I]);
    }
    if (CppIncludes.empty())
      continue;

    bool ClangCL = isClangCLMode(*Cmd);
    UnityEntry Entry;
    Entry.UnityPath = NormFile;
    Entry.OwnerStdafx = Includes.front();
    Entry.BaseCmd = stripTargetAndOutputArgs(*Cmd, NormFile, ClangCL);
    Entry.ClangCL = ClangCL;
    Entry.ExtraIncDirs = std::move(IncDirs);
    Entry.IncludedCpps = std::move(CppIncludes);

    AllStdafxSet.insert(Entry.OwnerStdafx);
    UnityEntries.push_back(std::move(Entry));
  }

  // Record scanned root even if no unity files exist so ensureInitializedFor
  // does not repeatedly block on non-unity projects.
  if (UnityEntries.empty()) {
    bool FirstForRoot = false;
    {
      std::unique_lock<std::shared_mutex> Lock(Mu);
      if (!SourceRoot.empty())
        FirstForRoot = KnownSourceRoots.insert(normalizePath(SourceRoot)).second;
    }
    if (FirstForRoot)
      OnPCHAnnounce.broadcast({});
    return;
  }

  for (UnityEntry &U : UnityEntries)
    StdafxToRepresentativeUnity.try_emplace(U.OwnerStdafx, &U);

  // Compile skip-deps regexes once.
  std::vector<llvm::Regex> SkipRegexes;
  for (const std::string &Pat : Cfg.SkipDeps)
    SkipRegexes.emplace_back(Pat);
  auto IsSkipped = [&](PathRef P) -> bool {
    for (const auto &R : SkipRegexes) {
      if (R.match(P))
        return true;
    }
    return false;
  };

  // Phase 2: Scan each stdafx.hpp to discover parent PCH links & headers.
  llvm::StringMap<std::string> ParentPCHMap; // stdafx -> parent stdafx
  llvm::StringMap<std::vector<std::string>> StdafxOwnedHeaders;

  for (auto &Pair : StdafxToRepresentativeUnity) {
    const std::string &Stdafx = Pair.first().str();
    const UnityEntry &U = *Pair.second;
    auto VFS = TFS.view(U.BaseCmd.Directory);

    std::vector<std::string> AllowedDirs;
    AllowedDirs.push_back(
        normalizePath(llvm::sys::path::parent_path(Stdafx)));
    if (auto It = Cfg.AllowIncludesFrom.find(Stdafx);
        It != Cfg.AllowIncludesFrom.end()) {
      for (const std::string &D : It->second)
        AllowedDirs.push_back(D);
    }

    llvm::StringSet<> Visited;
    std::vector<std::string> OwnedHeaders;
    auto CollectRecursive = [&](auto &Self, PathRef CurFile) -> void {
      if (!Visited.insert(CurFile).second)
        return;
      std::vector<std::string> Incs =
          scanQuotedIncludes(CurFile, *VFS, U.ExtraIncDirs);
      for (const std::string &Inc : Incs) {
        if (AllStdafxSet.contains(Inc)) {
          if (Inc != Stdafx && ParentPCHMap.find(Stdafx) == ParentPCHMap.end())
            ParentPCHMap[Stdafx] = Inc;
          continue; // Do not descend into parent stdafx.
        }
        if (isInAnyAllowedDir(Inc, AllowedDirs)) {
          Self(Self, Inc);
          if (isHeaderExtension(Inc) && !IsSkipped(Inc))
            OwnedHeaders.push_back(Inc);
        }
      }
    };
    CollectRecursive(CollectRecursive, Stdafx);
    StdafxOwnedHeaders[Stdafx] = std::move(OwnedHeaders);
  }

  // Phase 3: Topologically order static PCHs (L1 -> L2 -> L3).
  std::vector<std::string> OrderedStdafxs;
  llvm::StringSet<> Emitted;
  auto EmitTopo = [&](auto &Self, const std::string &S) -> void {
    if (Emitted.contains(S))
      return;
    if (auto It = ParentPCHMap.find(S); It != ParentPCHMap.end())
      Self(Self, It->second);
    if (Emitted.insert(S).second)
      OrderedStdafxs.push_back(S);
  };
  for (auto &Pair : StdafxToRepresentativeUnity)
    EmitTopo(EmitTopo, Pair.first().str());

  // Build new maps off-lock.
  llvm::StringMap<std::string> NewUnityToPCH;
  llvm::StringMap<CppMapping> NewCppToInfo;
  llvm::StringMap<HeaderMapping> NewHeaderToInfo;
  llvm::StringMap<PCHSpec> NewStaticPCHSpecs;
  llvm::StringMap<DirFallback> NewDirToFallback;
  std::vector<PCHSpec> AnnouncedStaticPCHs;

  for (const std::string &Stdafx : OrderedStdafxs) {
    const UnityEntry &U = *StdafxToRepresentativeUnity[Stdafx];
    PCHSpec Spec;
    Spec.IsDynamic = false;
    Spec.PCHHeader = Stdafx;
    if (auto It = ParentPCHMap.find(Stdafx); It != ParentPCHMap.end())
      Spec.ParentPCHHeader = It->second;
    Spec.BuildCommand = U.BaseCmd;
    Spec.BuildCommand.Filename = Stdafx;
    if (!Spec.ParentPCHHeader.empty())
      appendIncludeArg(Spec.BuildCommand.CommandLine, Spec.ParentPCHHeader,
                       U.ClangCL);
    appendHeaderTypeAndTarget(Spec.BuildCommand.CommandLine, Stdafx, U.ClangCL);

    AnnouncedStaticPCHs.push_back(Spec);
    NewStaticPCHSpecs[Stdafx] = std::move(Spec);
  }

  for (const UnityEntry &U : UnityEntries) {
    NewUnityToPCH[U.UnityPath] = U.OwnerStdafx;
    for (const std::string &Cpp : U.IncludedCpps) {
      tooling::CompileCommand CppCmd = U.BaseCmd;
      CppCmd.Filename = Cpp;
      appendIncludeArg(CppCmd.CommandLine, U.OwnerStdafx, U.ClangCL);
      appendCppTarget(CppCmd.CommandLine, Cpp, U.ClangCL);
      NewCppToInfo.try_emplace(Cpp, CppMapping{std::move(CppCmd), U.OwnerStdafx});

      std::string Dir = normalizePath(llvm::sys::path::parent_path(Cpp));
      NewDirToFallback.try_emplace(
          Dir, DirFallback{U.BaseCmd, U.BaseCmd.Directory, U.OwnerStdafx});
    }
  }

  for (const auto &Pair : StdafxOwnedHeaders) {
    const std::string &Stdafx = Pair.first().str();
    const UnityEntry &U = *StdafxToRepresentativeUnity[Stdafx];
    std::string ParentPCH;
    if (auto It = ParentPCHMap.find(Stdafx); It != ParentPCHMap.end())
      ParentPCH = It->second;

    std::string StdafxDir = normalizePath(llvm::sys::path::parent_path(Stdafx));
    NewDirToFallback.try_emplace(
        StdafxDir, DirFallback{U.BaseCmd, U.BaseCmd.Directory, Stdafx});

    for (const std::string &H : Pair.second) {
      tooling::CompileCommand HdrCmd = U.BaseCmd;
      HdrCmd.Filename = H;
      appendHeaderTypeAndTarget(HdrCmd.CommandLine, H, U.ClangCL);

      PCHSpec DynSpec;
      DynSpec.IsDynamic = true;
      DynSpec.PCHHeader = H;
      DynSpec.ParentPCHHeader = ParentPCH;
      DynSpec.UmbrellaHeader = Stdafx;
      DynSpec.StopBeforeHeader = H;
      DynSpec.BuildCommand = U.BaseCmd;
      DynSpec.BuildCommand.Filename = H + ".__clangd_ghost__.cpp";
      if (!ParentPCH.empty())
        appendIncludeArg(DynSpec.BuildCommand.CommandLine, ParentPCH,
                         U.ClangCL);
      appendIncludeArg(DynSpec.BuildCommand.CommandLine, Stdafx, U.ClangCL);
      appendHeaderTypeAndTarget(DynSpec.BuildCommand.CommandLine,
                                DynSpec.BuildCommand.Filename, U.ClangCL);

      NewHeaderToInfo.try_emplace(
          H, HeaderMapping{std::move(HdrCmd), std::move(DynSpec)});
    }
  }

  {
    std::unique_lock<std::shared_mutex> Lock(Mu);
    for (auto &P : NewUnityToPCH)
      UnityToPCH[P.first()] = std::move(P.second);
    for (auto &P : NewCppToInfo)
      CppToInfo[P.first()] = std::move(P.second);
    for (auto &P : NewHeaderToInfo)
      HeaderToInfo[P.first()] = std::move(P.second);
    for (auto &P : NewStaticPCHSpecs)
      StaticPCHSpecs[P.first()] = std::move(P.second);
    for (auto &P : NewDirToFallback)
      DirToFallback.try_emplace(P.first(), std::move(P.second));
    for (const std::string &S : OrderedStdafxs)
      TrackedUmbrellas.insert(S);
    if (!SourceRoot.empty())
      KnownSourceRoots.insert(normalizePath(SourceRoot));
    for (const std::string &F : Files)
      LastScannedFiles.push_back(F);
  }

  log("(UnityCDB) Discovered {0} static PCHs, {1} unity TUs, {2} .cpp files, "
      "{3} headers",
      AnnouncedStaticPCHs.size(), UnityEntries.size(), NewCppToInfo.size(),
      NewHeaderToInfo.size());
  OnPCHAnnounce.broadcast(std::move(AnnouncedStaticPCHs));
}

void UnityProjectCDB::notifyFileChanged(PathRef File) {
  std::string Norm = normalizePath(File);
  std::vector<std::string> ToRescan;
  {
    std::shared_lock<std::shared_mutex> Lock(Mu);
    if (!TrackedUmbrellas.contains(Norm) && !UnityToPCH.contains(Norm))
      return;
    ToRescan = LastScannedFiles;
  }
  if (!ToRescan.empty()) {
    log("(UnityCDB) File {0} changed; refreshing unity/PCH mappings", Norm);
    scanFiles(ToRescan);
  }
}

void UnityProjectCDB::ensureInitializedFor(PathRef File) const {
  if (!Opts.Enabled || !Base)
    return;
  if (auto PI = Base->getProjectInfo(File)) {
    std::string Root = normalizePath(PI->SourceRoot);
    {
      std::shared_lock<std::shared_mutex> Lock(Mu);
      if (KnownSourceRoots.contains(Root))
        return;
    }
    // Trigger broadcast discovery in Base and wait for onBaseCommandsChanged.
    (void)Base->getCompileCommand(File);
    Base->blockUntilIdle(Deadline::infinity());
  }
}

std::optional<tooling::CompileCommand>
UnityProjectCDB::getCompileCommand(PathRef File) const {
  std::string Norm = normalizePath(File);
  {
    std::shared_lock<std::shared_mutex> Lock(Mu);
    if (auto It = CppToInfo.find(Norm); It != CppToInfo.end())
      return It->second.Command;
    if (auto It = HeaderToInfo.find(Norm); It != HeaderToInfo.end())
      return It->second.Command;
    if (auto It = StaticPCHSpecs.find(Norm); It != StaticPCHSpecs.end())
      return It->second.BuildCommand;
  }

  ensureInitializedFor(File);

  {
    std::shared_lock<std::shared_mutex> Lock(Mu);
    if (auto It = CppToInfo.find(Norm); It != CppToInfo.end())
      return It->second.Command;
    if (auto It = HeaderToInfo.find(Norm); It != HeaderToInfo.end())
      return It->second.Command;
    if (auto It = StaticPCHSpecs.find(Norm); It != StaticPCHSpecs.end())
      return It->second.BuildCommand;

    std::string Dir = normalizePath(llvm::sys::path::parent_path(Norm));
    if (auto It = DirToFallback.find(Dir); It != DirToFallback.end()) {
      tooling::CompileCommand Cmd = It->second.BaseCommand;
      bool ClangCL = isClangCLMode(Cmd);
      Cmd.Filename = Norm;
      if (isHeaderExtension(Norm)) {
        appendHeaderTypeAndTarget(Cmd.CommandLine, Norm, ClangCL);
      } else {
        appendIncludeArg(Cmd.CommandLine, It->second.OwnerStdafx, ClangCL);
        appendCppTarget(Cmd.CommandLine, Norm, ClangCL);
      }
      return Cmd;
    }
  }

  return Base ? Base->getCompileCommand(File) : std::nullopt;
}

std::optional<PCHSpec> UnityProjectCDB::getPCHSpec(PathRef File) const {
  std::string Norm = normalizePath(File);
  auto Lookup = [&]() -> std::optional<PCHSpec> {
    std::shared_lock<std::shared_mutex> Lock(Mu);
    if (auto It = StaticPCHSpecs.find(Norm); It != StaticPCHSpecs.end()) {
      if (!It->second.ParentPCHHeader.empty()) {
        if (auto PIt = StaticPCHSpecs.find(It->second.ParentPCHHeader);
            PIt != StaticPCHSpecs.end())
          return PIt->second;
      }
      return std::nullopt;
    }
    if (auto It = UnityToPCH.find(Norm); It != UnityToPCH.end()) {
      if (auto SIt = StaticPCHSpecs.find(It->second);
          SIt != StaticPCHSpecs.end())
        return SIt->second;
    }
    if (auto It = CppToInfo.find(Norm); It != CppToInfo.end()) {
      if (auto SIt = StaticPCHSpecs.find(It->second.OwnerStdafx);
          SIt != StaticPCHSpecs.end())
        return SIt->second;
    }
    if (auto It = HeaderToInfo.find(Norm); It != HeaderToInfo.end())
      return It->second.Spec;

    std::string Dir = normalizePath(llvm::sys::path::parent_path(Norm));
    if (auto It = DirToFallback.find(Dir); It != DirToFallback.end()) {
      if (auto SIt = StaticPCHSpecs.find(It->second.OwnerStdafx);
          SIt != StaticPCHSpecs.end())
        return SIt->second;
    }
    return std::nullopt;
  };

  if (auto Res = Lookup())
    return Res;

  ensureInitializedFor(File);
  if (auto Res = Lookup())
    return Res;
  return Base ? Base->getPCHSpec(File) : std::nullopt;
}

std::optional<ProjectInfo> UnityProjectCDB::getProjectInfo(PathRef File) const {
  return Base ? Base->getProjectInfo(File) : std::nullopt;
}

std::unique_ptr<ProjectModules>
UnityProjectCDB::getProjectModules(PathRef File) const {
  return Base ? Base->getProjectModules(File) : nullptr;
}

tooling::CompileCommand
UnityProjectCDB::getFallbackCommand(PathRef File) const {
  return Base ? Base->getFallbackCommand(File)
              : GlobalCompilationDatabase::getFallbackCommand(File);
}

bool UnityProjectCDB::blockUntilIdle(Deadline D) const {
  return Base ? Base->blockUntilIdle(D) : true;
}

} // namespace clangd
} // namespace clang
