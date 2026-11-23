//===--- PCHManager.h - Hierarchical & Dynamic PCH Manager -------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PCHMANAGER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PCHMANAGER_H

#include "Compiler.h"
#include "Diagnostics.h"
#include "GlobalCompilationDatabase.h"
#include "Headers.h"
#include "index/CanonicalIncludes.h"
#include "support/Function.h"
#include "support/Path.h"
#include "support/Threading.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Threading.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace clang {
namespace clangd {

class ParsingCallbacks;

/// Priority queue for PCH build and invalidation tasks.
class PCHQueue {
public:
  struct Task {
    explicit Task(std::function<void()> Run) : Run(std::move(Run)) {}

    std::function<void()> Run;
    llvm::ThreadPriority ThreadPri = llvm::ThreadPriority::Background;
    unsigned QueuePri = 0;

    bool operator<(const Task &O) const { return QueuePri < O.QueuePri; }
  };

  struct Stats {
    unsigned Enqueued = 0;
    unsigned Active = 0;
    unsigned Completed = 0;
    unsigned LastIdle = 0;
  };

  PCHQueue(std::function<void(Stats)> OnProgress = nullptr)
      : OnProgress(OnProgress) {}

  void push(Task);
  void append(std::vector<Task>);
  void work(std::function<void()> OnIdle = nullptr);
  void stop();

private:
  void notifyProgress() const;

  std::mutex Mu;
  Stats Stat;
  std::condition_variable CV;
  bool ShouldStop = false;
  std::vector<Task> Queue;
  std::function<void(Stats)> OnProgress;
};

/// Manages hierarchical static PCHs (stdafx.hpp chains) and in-memory dynamic
/// header PCHs (prefix-truncated umbrella PCHs for non-self-contained headers).
class PCHManager {
  using FSType = IntrusiveRefCntPtr<llvm::vfs::FileSystem>;
  using uniq_lck = std::unique_lock<std::shared_timed_mutex>;
  using shared_lck = std::shared_lock<std::shared_timed_mutex>;
  struct PCHItem;
  using shared_pch_item = std::shared_ptr<PCHItem>;
  using weak_pch_item = std::weak_ptr<PCHItem>;
  using PCHDataType = std::shared_ptr<const llvm::MemoryBuffer>;
  using UsedPCHDataList = std::vector<PCHDataType>;

  struct PCHSnapshot {
    int Version = 0;
    std::string Filename;
    PCHDataType PCHData;
    std::vector<shared_pch_item> PCHItems;
    UsedPCHDataList UsedPCHDatasSnapshot;
    IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> MemFS;
  };
  using PCHSnapshotPtr = std::shared_ptr<PCHSnapshot>;

  struct PCHItem {
    enum class State { Valid, Rebuild, Invalid };

    struct IncFileState {
      size_t Size = 0;
      llvm::sys::TimePoint<> ModTime;
      llvm::MD5::MD5Result MD5{};

      IncFileState() = default;
      IncFileState(const llvm::vfs::Status &S);

      bool operator==(const IncFileState &Rhs) const {
        return Size == Rhs.Size && ModTime == Rhs.ModTime;
      }
      bool operator!=(const IncFileState &Rhs) const {
        return !(*this == Rhs);
      }

      bool compare(StringRef Path, const llvm::vfs::Status &S,
                   bool CheckMD5);

      static std::optional<IncFileState> read(StringRef &Data);
      void write(llvm::raw_ostream &OS) const;
    };

    PCHItem(PCHSpec S) : Spec(std::move(S)) {}

    unsigned invalidate();
    bool isIncludeStateDifferent(StringRef Path, FSType &VFS,
                                 bool CheckMD5);
    bool isAnyIncludeStateDifferent(FSType &VFS, PathRef WorkspaceRoot,
                                    PathRef SourceRoot);
    void updateIncludeStates(FSType &VFS, PathRef WorkspaceRoot,
                             PathRef SourceRoot);

    static bool storeTo(llvm::raw_ostream &OS, const PCHItem &I,
                        uint64_t CommandHash);
    struct DepV {
      std::string FName;
      uint32_t Version = 0;
    };
    using DependencyVersions = std::vector<DepV>;
    static bool read(StringRef &Data, PCHItem &I, DependencyVersions &Deps,
                     uint64_t ExpectedCmdHash);

    PCHSpec Spec;
    PCHDataType PCHData;
    std::vector<weak_pch_item> DependOnMe;
    std::vector<shared_pch_item> IdependOn;

    std::vector<std::string> Includes;
    llvm::StringMap<IncFileState> IncludeStates;
    llvm::StringSet<> DynamicIncludes;

    State ItemState = State::Rebuild;
    int Version = 0;
    bool Dynamic = false;
    uint64_t LastUsedSeq = 0;

    mutable std::shared_timed_mutex Lock;
    mutable std::condition_variable_any CV;
    mutable PCHSnapshotPtr PCHDatasSnapshot;
  };

public:
  struct Stats {
    unsigned Completed = 0;
    unsigned Total = 0;
  };

  struct Options {
    bool StorePCHInMemory = true;
    bool WaitForInit = true;
    std::function<void(Stats)> OnProgress;
    std::optional<std::string> WorkspaceRoot;
    std::string ClangdCacheDir;
    unsigned MaxConcurrency = 0;
    size_t MaxDynamicPCHs = 24;
    bool DbgLog = false;
  };

  struct PCHEvent {
    PathRef PCHPath;
    bool Success;
  };
  using PCHBuiltEvent = Event<PCHEvent>;

  PCHManager(const GlobalCompilationDatabase &CDB, const ThreadsafeFS &TFS,
             ParsingCallbacks &Callbacks, const Options &Opts);
  ~PCHManager();

  class PCHAccess {
  public:
    PCHAccess() = default;
    PCHAccess(const PCHAccess &) = delete;
    PCHAccess(PCHAccess &&) noexcept = default;
    ~PCHAccess() = default;

    PCHAccess &operator=(const PCHAccess &) = delete;
    PCHAccess &operator=(PCHAccess &&) noexcept = default;

    bool addPCH(CompilerInvocation *CI, FSType &VFS) const;
    llvm::StringRef filename() const { return ItemSnapshot->Filename; }
    int version() const { return ItemSnapshot->Version; }

    explicit operator bool() const { return ItemSnapshot != nullptr; }

    PCHManager *getManager() const { return Manager; }

  private:
    PCHAccess(PCHSnapshotPtr Snapshot, shared_pch_item ShItem, PCHManager *Mgr)
        : ShItem(std::move(ShItem)), Manager(Mgr),
          ItemSnapshot(std::move(Snapshot)) {}

    shared_pch_item ShItem;
    PCHManager *Manager = nullptr;
    PCHSnapshotPtr ItemSnapshot;
    friend class PCHManager;
  };

  std::optional<std::string> getPCHCacheDirFor(PathRef File) const;

  void checkChangedFile(PathRef File, FSType FS, bool Force = false);

  PCHAccess findPCH(PathRef File, const tooling::CompileCommand &Cmd) const;
  PCHAccess findPCH(PathRef PCHFile) const;
  PCHAccess findDynPCH(PathRef PCHFile) const;

  PCHAccess tryFindPCH(PathRef File, const tooling::CompileCommand &Cmd) const;
  PCHAccess tryFindPCH(PathRef PCHFile) const;

  bool hasPCHInDependencies(PathRef File, const tooling::CompileCommand &Cmd,
                            PathRef PCHFile) const;

  PCHBuiltEvent::Subscription watch(PCHBuiltEvent::Listener L) const {
    return OnPCHBuilt.observe(std::move(L));
  }

  bool tryAddDynamicPCH(PathRef File, FSType FS);
  bool tryRemoveDynamicPCH(PathRef File);

  static bool DbgLog;

private:
  void enqueue(std::vector<std::string> ChangedFiles, FSType FS);
  void enqueue(std::vector<PCHSpec> AnnouncedPCHs);

  PCHQueue::Task changedFilesTask(std::vector<std::string> ChangedFiles,
                                  FSType FS);
  PCHQueue::Task announcedPCHTask(std::vector<PCHSpec> AnnouncedPCHs);

  void debounceLoop();
  void analyzePCHDependencies(std::vector<PCHSpec> PCHSpecs);
  unsigned invalidateAffectedPCH(const std::vector<std::string> &ChangedFiles,
                                 FSType FS);
  void rebuildInvalidatedPCH(unsigned Total, FSType FS);
  void updateAllHeaders();
  void rebuildPCH(shared_pch_item Item, FSType FS);
  void evictDynamicPCHsIfNeeded();

  static void makeSnapshot(const shared_pch_item &Item,
                           const PCHSnapshotPtr &Snap);
  static void addDynamicGhost(const shared_pch_item &Item,
                              IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> MemFS);
  static uint64_t computeCommandHash(const tooling::CompileCommand &Cmd);

  const GlobalCompilationDatabase &CDB;
  const ThreadsafeFS &TFS;
  ParsingCallbacks &Callbacks;
  std::function<void(Stats)> OnProgress;

  PCHQueue Queue;

  bool WaitForInit = true;
  std::atomic<bool> Initialized{false};
  mutable std::condition_variable_any InitCV;

  std::vector<shared_pch_item> PCHs;
  llvm::StringMap<shared_pch_item> StaticPCHMap;
  mutable std::shared_timed_mutex PCHLock;

  llvm::StringMap<shared_pch_item> DynamicPCHs;
  mutable std::shared_timed_mutex DynamicPCHLock;
  mutable std::atomic<uint64_t> DynamicUseSeq{0};

  std::optional<std::string> WorkspaceRoot;
  std::string ClangdCacheDir;
  unsigned MaxConcurrency = 4;
  size_t MaxDynamicPCHs = 24;

  std::mutex ChangedMtx;
  std::condition_variable ChangedCV;
  bool StopDebouncer = false;
  std::vector<std::string> Changed;
  FSType ChangedFS;
  std::chrono::steady_clock::time_point ChangedLastTime;

  llvm::StringSet<> AllUsedHeaders;
  mutable std::shared_timed_mutex UsedHeadersLock;

  unsigned Total = 0;
  std::atomic<unsigned> Complete{0};

  mutable PCHBuiltEvent OnPCHBuilt;

  AsyncTaskRunner ThreadPool;
  AsyncTaskRunner CacheWritePool;
  std::thread DebounceThread;

  // Must be declared last so CDB subscriptions disconnect before teardown.
  GlobalCompilationDatabase::CommandChanged::Subscription CommandsChanged;
  GlobalCompilationDatabase::PCHAnnounce::Subscription PCHAnnounce;
};

class PPSkipIncludes : public PPCallbacks {
  std::string Target;
  std::string TargetNorm;
  bool SkipTarget;

  bool Skipped = false;
  std::optional<FileEntryRef> TargetRef;
  llvm::StringSet<> AllowedIncludes;

public:
  PPSkipIncludes(SourceManager &SM, StringRef TargetFile, bool SkipTarget);

  bool wasSkipped() const { return Skipped; }
  StringRef getTarget() const { return Target; }
  llvm::StringSet<> takeAllowedIncludes() { return std::move(AllowedIncludes); }

  bool InclusionAllowed(SourceLocation HashLoc, const Token &IncludeTok,
                        StringRef FileName, bool IsAngled,
                        CharSourceRange FilenameRange,
                        OptionalFileEntryRef File, StringRef SearchPath,
                        StringRef RelativePath, const Module *Imported,
                        SrcMgr::CharacteristicKind FileType) override;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PCHMANAGER_H
