//===--- UnityProjectCDB.h - Auto-discover unity/PCH hierarchies -*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_UNITYPROJECTCDB_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_UNITYPROJECTCDB_H

#include "GlobalCompilationDatabase.h"
#include "support/Path.h"
#include "support/ThreadsafeFS.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace clang {
namespace clangd {

/// Wraps a GlobalCompilationDatabase and automatically discovers unity-build
/// translation units and hierarchical stdafx-style precompiled headers from
/// unmodified standard compile_commands.json databases.
///
/// For non-unity codebases, behaves as a transparent pass-through.
class UnityProjectCDB : public GlobalCompilationDatabase {
public:
  struct Options {
    bool Enabled = true;
    std::optional<std::string> ConfigPath;
    std::optional<std::string> WorkspaceRoot;
  };

  UnityProjectCDB(
      const GlobalCompilationDatabase *Base, const ThreadsafeFS &TFS,
      Options Opts,
      std::optional<std::string> FallbackWorkingDirectory = std::nullopt);

  UnityProjectCDB(const GlobalCompilationDatabase *Base,
                  const ThreadsafeFS &TFS)
      : UnityProjectCDB(Base, TFS, Options{}) {}

  UnityProjectCDB(
      std::unique_ptr<GlobalCompilationDatabase> Base, const ThreadsafeFS &TFS,
      Options Opts,
      std::optional<std::string> FallbackWorkingDirectory = std::nullopt);

  UnityProjectCDB(std::unique_ptr<GlobalCompilationDatabase> Base,
                  const ThreadsafeFS &TFS)
      : UnityProjectCDB(std::move(Base), TFS, Options{}) {}

  std::optional<tooling::CompileCommand>
  getCompileCommand(PathRef File) const override;

  std::optional<ProjectInfo> getProjectInfo(PathRef File) const override;

  std::unique_ptr<ProjectModules>
  getProjectModules(PathRef File) const override;

  tooling::CompileCommand getFallbackCommand(PathRef File) const override;

  bool blockUntilIdle(Deadline D) const override;

  std::optional<PCHSpec> getPCHSpec(PathRef File) const override;
  void replayPCHAnnounce() const override;

  /// Explicitly scans \p Files (used by CDB broadcast watcher and unit tests).
  void scanFiles(llvm::ArrayRef<std::string> Files);

  /// Re-scans mappings if \p File is a tracked unity TU or umbrella header.
  void notifyFileChanged(PathRef File);

private:
  struct CppMapping {
    tooling::CompileCommand Command;
    std::string OwnerStdafx;
  };
  struct HeaderMapping {
    tooling::CompileCommand Command;
    PCHSpec Spec;
  };
  struct DirFallback {
    tooling::CompileCommand BaseCommand;
    std::string Directory;
    std::string OwnerStdafx;
  };

  struct ConfigOverrides {
    std::string ConfigPath;
    std::vector<std::string> FilterIn;
    std::vector<std::string> FilterOut;
    std::vector<std::string> SkipDeps;
    // Map from normalized stdafx path -> additional allowed include dirs.
    llvm::StringMap<std::vector<std::string>> AllowIncludesFrom;
    bool Loaded = false;
  };

  void onBaseCommandsChanged(const std::vector<std::string> &Files);
  void ensureInitializedFor(PathRef File) const;
  ConfigOverrides loadConfig(PathRef SourceRoot) const;

  const GlobalCompilationDatabase *Base;
  std::unique_ptr<GlobalCompilationDatabase> BaseOwner;
  const ThreadsafeFS &TFS;
  Options Opts;

  mutable std::shared_mutex Mu;
  llvm::StringMap<std::string> UnityToPCH;       // unity cpp -> owner stdafx
  llvm::StringMap<CppMapping> CppToInfo;         // included cpp -> cmd + stdafx
  llvm::StringMap<HeaderMapping> HeaderToInfo;   // header -> cmd + dynamic PCHSpec
  llvm::StringMap<PCHSpec> StaticPCHSpecs;       // stdafx -> static PCHSpec
  llvm::StringMap<DirFallback> DirToFallback;    // src/<lvl> dir -> fallback
  llvm::StringSet<> TrackedUmbrellas;            // all known stdafx paths
  llvm::StringSet<> KnownSourceRoots;            // roots already scanned
  std::vector<std::string> LastScannedFiles;     // for live re-scan on edits
  std::vector<PCHSpec> LastAnnouncedStaticPCHs;  // cached for late listeners
  std::string LoadedConfigPath;                  // for live config reload

  // Must be declared last so subscription disconnects before member teardown.
  CommandChanged::Subscription BaseChanged;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_UNITYPROJECTCDB_H
