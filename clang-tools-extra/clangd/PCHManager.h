#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PCHMANAGER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PCHMANAGER_H

#include "Headers.h"
#include "Compiler.h"
#include "Diagnostics.h"
#include "GlobalCompilationDatabase.h"
#include "index/CanonicalIncludes.h"
#include "support/Function.h"
#include "support/MemoryTree.h"
#include "support/Path.h"
#include "support/Threading.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Threading.h"
#include <chrono>
#include <memory>
#include <shared_mutex>

namespace clang {
namespace clangd {

class ParsingCallbacks;

unsigned getDefaultAsyncThreadsCount();

//Queue for PCH
class PCHQueue {
public:
  /// A work item on the thread pool's queue.
  struct Task {
    explicit Task(std::function<void()> Run) : Run(std::move(Run)) {}

    std::function<void()> Run;
    llvm::ThreadPriority ThreadPri = llvm::ThreadPriority::Background;
    unsigned QueuePri = 0; // Higher-priority tasks will run first.

    bool operator<(const Task &O) const { return QueuePri < O.QueuePri; }
  };

  // Describes the number of tasks processed by the queue.
  struct Stats {
    unsigned Enqueued = 0;  // Total number of tasks ever enqueued.
    unsigned Active = 0;    // Tasks being currently processed by a worker.
    unsigned Completed = 0; // Tasks that have been finished.
    unsigned LastIdle = 0;  // Number of completed tasks when last empty.
  };

  PCHQueue(std::function<void(Stats)> OnProgress = nullptr)
      : OnProgress(OnProgress) {}

  // Add tasks to the queue.
  void push(Task);
  void append(std::vector<Task>);

  // Process items on the queue until the queue is stopped.
  // If the queue becomes empty, OnIdle will be called (on one worker).
  void work(std::function<void()> OnIdle = nullptr);

  // Stop processing new tasks, allowing all work() calls to return soon.
  void stop();
private:
  void notifyProgress() const; // Requires lock Mu
  bool adjust(Task &T);

  std::mutex Mu;
  Stats Stat;
  std::condition_variable CV;
  bool ShouldStop = false;
  std::vector<Task> Queue; // max-heap
  std::function<void(Stats)> OnProgress;
};

class PCHManager {
    using uniq_lck = std::unique_lock<std::shared_timed_mutex>;
    struct PCHItem
    {
      enum class State {Valid, Rebuild, Invalid};

        PCHItem(tooling::CompileCommand CC): CompileCommand(std::move(CC)), PCHData(std::make_shared<std::string>()) {}
        unsigned invalidate(uniq_lck &Lock, bool WaitForNoUsage);

        tooling::CompileCommand CompileCommand;
        std::shared_ptr<std::string> PCHData;
        std::vector<PCHItem*> DependOnMe;
        std::vector<PCHItem*> IdependOn;
        IncludeStructure Includes;
        CanonicalIncludes CanonIncludes;
        State ItemState = State::Rebuild;
        int Version = 0;
        bool Dynamic = false;
        llvm::StringSet<> DynamicIncludes;
        mutable std::atomic<unsigned> InUse{0};
        mutable std::condition_variable_any CV;
    };
public:
  struct Stats {
    unsigned Completed = 0; // PCHs generated
    unsigned Total = 0;     // Total amount of PCHs
  };

  struct Options {
    /// Cache (large) pch data in RAM rather than temporary files on disk.
    bool StorePCHInMemory = true;
    bool WaitForInit = true;
    std::function<void(Stats)> OnProgress;
  };

  using shared_lck = std::shared_lock<std::shared_timed_mutex>;
  using UsedPCHDataList = std::vector<std::shared_ptr<std::string>>;
  struct PCHEvent {
    PathRef PCHPath;
    bool Success;
  };
  using PCHBuiltEvent = Event<PCHEvent>;

  PCHManager(const GlobalCompilationDatabase &CDB, const ThreadsafeFS &TFS, ParsingCallbacks &Callbacks, const Options &Opts);
  ~PCHManager();

  class PCHAccess 
  {
  public:
    PCHAccess() = default;
    PCHAccess(const PCHAccess &) = delete;
    PCHAccess(PCHAccess &&);
    ~PCHAccess();

    PCHAccess& operator=(const PCHAccess &) = delete;
    PCHAccess& operator=(PCHAccess &&);

    bool addPCH(CompilerInvocation *CI, IntrusiveRefCntPtr<llvm::vfs::FileSystem> &VFS) const;
    llvm::StringRef filename() const {return Item->CompileCommand.Filename;}
    auto version() const { return Item->Version; }

    operator bool() const { return Item != nullptr; }

    PCHManager *getManager() const { return pManager; }
  private:
    PCHAccess(const PCHItem *Item, PCHManager *pMgr);
    PCHAccess(std::shared_ptr<PCHItem> ShItem, PCHManager *pMgr);

   std::shared_ptr<PCHItem> DynItem;
    const PCHItem *Item = nullptr;
    mutable UsedPCHDataList UsedPCHDatas;
    PCHManager *pManager = nullptr;
    friend class PCHManager;
  };

  using FSType = llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>;

  void checkChangedFile(PathRef File, FSType FS);

  PCHAccess findPCH(tooling::CompileCommand const& Cmd) const;
  PCHAccess findPCH(clang::clangd::PathRef PCHFile) const;
  PCHAccess findDynPCH(clang::clangd::PathRef PCHFile) const;

  PCHAccess tryFindPCH(tooling::CompileCommand const& Cmd) const;
  PCHAccess tryFindDynPCH(tooling::CompileCommand const& Cmd) const;
  PCHAccess tryFindPCH(clang::clangd::PathRef PCHFile) const;

  bool hasPCHInDependencies(tooling::CompileCommand const& Cmd, PathRef PCHFile) const;

  PCHBuiltEvent::Subscription watch(PCHBuiltEvent::Listener L) const {
    return OnPCHBuilt.observe(std::move(L));
  }

  bool tryAddDynamicPCH(tooling::CompileCommand const &Cmd, FSType FS);
  bool tryRemoveDynamicPCH(tooling::CompileCommand const &Cmd);

  private:
    void enqueue(const std::vector<std::string> &ChangedFiles, FSType FS) {
        Queue.push(changedFilesTask(ChangedFiles, FS));
    }

    void enqueue(const std::vector<tooling::CompileCommand> &AnnouncedPCHs) {
        Queue.push(announcedPCHTask(AnnouncedPCHs));
    }

    PCHQueue::Task
    checkChangedPeriodically();

    PCHQueue::Task
    changedFilesTask(const std::vector<std::string> &ChangedFiles, FSType FS);

    PCHQueue::Task
    announcedPCHTask(const std::vector<tooling::CompileCommand> &AnnouncedPCHs);

    void analyzePCHDependencies(std::vector<tooling::CompileCommand> PCHCommands);
    unsigned invalidateAffectedPCH(const std::vector<std::string> &ChangedFiles);
    void rebuildInvalidatedPCH(unsigned Tota, FSType FSl);
    void updateAllHeaders();
    void rebuildPCH(PCHItem &Item, FSType FS);

    static IntrusiveRefCntPtr<llvm::vfs::FileSystem> addDependencies(const PCHItem *Dep, IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS, UsedPCHDataList &pchdatas);
    static void addDynamicGhost(const PCHItem *Dep, IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> MemFS);

    using CDBWeak = std::weak_ptr<const tooling::CompilationDatabase>;
    using PCHItemList = std::vector<std::unique_ptr<PCHItem>>;
    using PCHSharedItemMap = std::unordered_map<std::string, std::shared_ptr<PCHItem>>;

    const GlobalCompilationDatabase &CDB;
    const ThreadsafeFS &TFS;
    ParsingCallbacks &Callbacks;
    std::function<void(Stats)> OnProgress;

    PCHQueue Queue;
    GlobalCompilationDatabase::CommandChanged::Subscription CommandsChanged;
    GlobalCompilationDatabase::PCHAnnounce::Subscription PCHAnnounce;

    bool WaitForInit = true;
    std::atomic<bool> Initialized = {false};
    mutable std::condition_variable_any InitCV;
    PCHItemList PCHs;
    mutable std::shared_timed_mutex PCHLock;
    AsyncTaskRunner ThreadPool;

    PCHSharedItemMap DynamicPCHs;
    mutable std::shared_timed_mutex DynamicPCHLock;

    using clock_t = std::chrono::steady_clock;
    using time_point_t = std::chrono::time_point<clock_t>;
    std::mutex ChangedMtx;
    std::vector<std::string> Changed;
    FSType ChangedFS;
    time_point_t ChangedLastTime;

    llvm::StringSet<> AllUsedHeaders;
    mutable std::shared_timed_mutex UsedHeadersLock;

    unsigned Total=0;
    std::atomic<unsigned> Complete{0};

    mutable PCHBuiltEvent OnPCHBuilt;
};

class PPSkipIncludes : public PPCallbacks {
    StringRef m_Target;
    bool m_SkipTarget;

    bool m_Skipped = false;
    std::optional<FileEntryRef> m_TargetRef;
    llvm::StringSet<> m_AllowedIncludes;

  public:
    PPSkipIncludes(SourceManager &sm, StringRef target, bool skipTarget);
    bool WasSkipped() const { return m_Skipped; }
    StringRef GetTarget() const { return m_Target; }
    llvm::StringSet<> takeAllowedIncludes() { return std::move(m_AllowedIncludes); }

    virtual bool InclusionAllowed(SourceLocation HashLoc,
                                  const Token &IncludeTok, StringRef FileName,
                                  bool IsAngled, CharSourceRange FilenameRange,
                                  OptionalFileEntryRef File,
                                  StringRef SearchPath, StringRef RelativePath,
                                  const Module *Imported,
                                  SrcMgr::CharacteristicKind FileType) override;
    static PPSkipIncludes *CheckSkipIncludesArg(const tooling::CompileCommand &CC, clang::CompilerInstance *pCI);
};

} // namespace clangd
} // namespace clang

#endif