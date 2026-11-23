//===--- PCHManager.cpp - Hierarchical & Dynamic PCH Manager -----*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PCHManager.h"
#include "CompileCommands.h"
#include "Preamble.h"
#include "TUScheduler.h"
#include "support/Logger.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/Version.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/PrecompiledPreamble.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ASTWriter.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <shared_mutex>
#include <string>
#include <utility>

namespace clang {
namespace clangd {
namespace {

constexpr char kPCHCacheMagic[4] = {'C', 'P', 'C', 'H'};
constexpr uint32_t kPCHCacheSchemaVersion = 2;

std::string normalizeKey(PathRef P) {
  llvm::SmallString<256> Buf(P);
  llvm::sys::path::native(Buf);
  llvm::sys::path::remove_dots(Buf, /*remove_dot_dot=*/true);
  return normalizePathDriveLetter(Buf);
}

bool isWorkspaceHeader(PathRef Header, PathRef WorkspaceRoot,
                       PathRef SourceRoot) {
  if (!llvm::sys::path::is_absolute(Header))
    return true;
  if (!WorkspaceRoot.empty() && pathStartsWith(WorkspaceRoot, Header))
    return true;
  if (!SourceRoot.empty() && pathStartsWith(SourceRoot, Header))
    return true;
  return false;
}

class PrecompilePCHAction : public ASTFrontendAction {
public:
  PrecompilePCHAction(SmallVectorImpl<char> *InMemStorage,
                      PreambleCallbacks &Callbacks)
      : InMemStorage(InMemStorage), Callbacks(Callbacks) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef InFile) override;

  bool hasEmittedPreamblePCH() const { return HasEmittedPreamblePCH; }

  void setEmittedPreamblePCH(ASTWriter &Writer) {
    HasEmittedPreamblePCH = true;
    Callbacks.AfterPCHEmitted(Writer);
  }

  bool BeginSourceFileAction(CompilerInstance &CI) override {
    assert(CI.getLangOpts().CompilingPCH);
    return ASTFrontendAction::BeginSourceFileAction(CI);
  }

  bool shouldEraseOutputFiles() override { return !hasEmittedPreamblePCH(); }
  bool hasCodeCompletionSupport() const override { return false; }
  bool hasASTFileSupport() const override { return false; }
  TranslationUnitKind getTranslationUnitKind() override { return TU_Prefix; }

private:
  friend class PrecompilePCHConsumer;
  bool HasEmittedPreamblePCH = false;
  SmallVectorImpl<char> *InMemStorage;
  PreambleCallbacks &Callbacks;
};

class PrecompilePCHConsumer : public PCHGenerator {
public:
  PrecompilePCHConsumer(PrecompilePCHAction &Action, Preprocessor &PP,
                        ModuleCache &ModCache, StringRef Isysroot,
                        const CodeGenOptions &CodeGenOpts,
                        SmallVectorImpl<char> *InMemStorage)
      : PCHGenerator(PP, ModCache, "", Isysroot, std::make_shared<PCHBuffer>(),
                     CodeGenOpts,
                     ArrayRef<std::shared_ptr<ModuleFileExtension>>(),
                     /*AllowASTWithErrors=*/true),
        Action(Action), InMemStorage(InMemStorage) {}

  bool HandleTopLevelDecl(DeclGroupRef DG) override {
    Action.Callbacks.HandleTopLevelDecl(DG);
    return true;
  }

  void HandleTranslationUnit(ASTContext &Ctx) override {
    PCHGenerator::HandleTranslationUnit(Ctx);
    if (!hasEmittedPCH())
      return;

    *InMemStorage = std::move(getPCH());
    Action.setEmittedPreamblePCH(getWriter());
  }

  bool shouldSkipFunctionBody(Decl *D) override {
    return Action.Callbacks.shouldSkipFunctionBody(D);
  }

private:
  PrecompilePCHAction &Action;
  SmallVectorImpl<char> *InMemStorage;
};

std::unique_ptr<ASTConsumer>
PrecompilePCHAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  std::string Sysroot;
  if (!GeneratePCHAction::ComputeASTConsumerArguments(CI, Sysroot))
    return nullptr;

  if (!CI.getFrontendOpts().RelocatablePCH)
    Sysroot.clear();

  return std::make_unique<PrecompilePCHConsumer>(
      *this, CI.getPreprocessor(), CI.getModuleCache(), Sysroot,
      CI.getCodeGenOpts(), InMemStorage);
}

class CppFilePreambleCallbacks : public PreambleCallbacks {
public:
  CppFilePreambleCallbacks(PathRef File, PreambleParsedCallback ParsedCallback)
      : File(File), ParsedCallback(std::move(ParsedCallback)) {}

  IncludeStructure takeIncludes() { return std::move(Includes); }
  include_cleaner::PragmaIncludes takePragmaIncludes() {
    return std::move(Pragmas);
  }

  void AfterExecute(CompilerInstance &CI) override {
    if (!ParsedCallback)
      return;
    if (CI.getASTReader()) {
      CI.getASTReader()->setDeserializationListener(nullptr);
      CI.getASTReader()->StartTranslationUnit(nullptr);
    }
    CI.getASTContext().setASTMutationListener(nullptr);
    trace::Span Tracer("Running PreambleCallback");
    ParsedCallback(CapturedASTCtx(CI),
                   std::make_shared<const include_cleaner::PragmaIncludes>(
                       takePragmaIncludes()));
  }

  void BeforeExecute(CompilerInstance &CI) override {
    CanonIncludes.addSystemHeadersMapping(CI.getLangOpts());
    Pragmas.record(CI);
  }

  void InitiateIncludeCollection(CompilerInstance &CI) {
    Includes.collect(CI);
  }

private:
  PathRef File;
  PreambleParsedCallback ParsedCallback;
  IncludeStructure Includes;
  CanonicalIncludes CanonIncludes;
  include_cleaner::PragmaIncludes Pragmas;
};

} // namespace

bool PCHManager::DbgLog = false;

PCHManager::PCHManager(const GlobalCompilationDatabase &CDB,
                       const ThreadsafeFS &TFS, ParsingCallbacks &Callbacks,
                       const Options &Opts)
    : CDB(CDB), TFS(TFS), Callbacks(Callbacks),
      OnProgress(Opts.OnProgress), WaitForInit(Opts.WaitForInit),
      WorkspaceRoot(Opts.WorkspaceRoot), ClangdCacheDir(Opts.ClangdCacheDir),
      MaxConcurrency(Opts.MaxConcurrency
                         ? Opts.MaxConcurrency
                         : std::clamp(std::thread::hardware_concurrency() / 2,
                                      2u, 8u)),
      MaxDynamicPCHs(Opts.MaxDynamicPCHs) {
  DbgLog = Opts.DbgLog;
  ThreadPool.runAsync("pch-worker",
                      [this, Ctx(Context::current().clone())]() mutable {
                        WithContext BGContext(std::move(Ctx));
                        Queue.work();
                      });
  DebounceThread = std::thread([this, Ctx(Context::current().clone())]() mutable {
    WithContext BGContext(std::move(Ctx));
    debounceLoop();
  });
  CommandsChanged =
      CDB.watch([this](const std::vector<std::string> &ChangedFiles) {
        enqueue(ChangedFiles, FSType());
      });
  PCHAnnounce =
      CDB.watch([this](const std::vector<PCHSpec> &Specs) { enqueue(Specs); });
}

PCHManager::~PCHManager() {
  // Subscriptions must be destroyed or stopped before Queue/ThreadPools.
  CommandsChanged = {};
  PCHAnnounce = {};
  {
    std::lock_guard<std::mutex> Lock(ChangedMtx);
    StopDebouncer = true;
  }
  ChangedCV.notify_all();
  if (DebounceThread.joinable())
    DebounceThread.join();
  Queue.stop();
  ThreadPool.wait();
  CacheWritePool.wait();
}

void PCHManager::enqueue(std::vector<std::string> ChangedFiles, FSType FS) {
  Queue.push(changedFilesTask(std::move(ChangedFiles), std::move(FS)));
}

void PCHManager::enqueue(std::vector<PCHSpec> AnnouncedPCHs) {
  Queue.push(announcedPCHTask(std::move(AnnouncedPCHs)));
}

PCHQueue::Task
PCHManager::changedFilesTask(std::vector<std::string> ChangedFiles, FSType FS) {
  PCHQueue::Task T([this, Files = std::move(ChangedFiles),
                    FS = std::move(FS)]() mutable {
    unsigned Invalidated = invalidateAffectedPCH(Files, FS);
    rebuildInvalidatedPCH(Invalidated, FS);
    updateAllHeaders();
  });
  T.ThreadPri = llvm::ThreadPriority::Default;
  return T;
}

void PCHManager::debounceLoop() {
  using namespace std::chrono_literals;
  std::unique_lock<std::mutex> Lock(ChangedMtx);
  while (true) {
    ChangedCV.wait(Lock, [&] { return StopDebouncer || !Changed.empty(); });
    if (StopDebouncer)
      return;

    auto TargetTime = ChangedLastTime + 2000ms;
    if (std::chrono::steady_clock::now() < TargetTime) {
      ChangedCV.wait_until(Lock, TargetTime,
                           [&] { return StopDebouncer; });
      if (StopDebouncer)
        return;
      // If more edits arrived while waiting, keep debouncing.
      if (std::chrono::steady_clock::now() < ChangedLastTime + 2000ms)
        continue;
    }

    std::vector<std::string> Batch = std::move(Changed);
    Changed.clear();
    FSType BatchFS = std::move(ChangedFS);
    ChangedFS = nullptr;
    Lock.unlock();

    if (DbgLog)
      log("(PCH) sending {0} debounced changed file(s) for processing",
          Batch.size());
    enqueue(std::move(Batch), std::move(BatchFS));

    Lock.lock();
  }
}

std::optional<std::string> PCHManager::getPCHCacheDirFor(PathRef File) const {
  std::string CacheName = ClangdCacheDir.empty() ? "clangd" : ClangdCacheDir;
  if (auto PI = CDB.getProjectInfo(File)) {
    llvm::SmallString<128> StorageDir(PI->SourceRoot);
    if (!PI->ClangdCacheDir.empty())
      CacheName = PI->ClangdCacheDir;
    llvm::sys::path::append(StorageDir, ".cache", CacheName, "pch");
    return std::string(StorageDir);
  }
  if (WorkspaceRoot && !WorkspaceRoot->empty()) {
    llvm::SmallString<128> StorageDir(*WorkspaceRoot);
    llvm::sys::path::append(StorageDir, ".cache", CacheName, "pch");
    return std::string(StorageDir);
  }
  return std::nullopt;
}

void PCHManager::checkChangedFile(PathRef File, FSType FS, bool Force) {
  if (!Initialized.load(std::memory_order_acquire)) {
    if (DbgLog)
      log("(PCH) checkChangedFile: not initialized yet");
    return;
  }

  std::string NormFile = normalizeKey(File);
  {
    shared_lck SharedAccess(UsedHeadersLock);
    if (!AllUsedHeaders.contains(NormFile)) {
      std::string Alt = normalizePathDriveLetter(NormFile, true);
      if (!AllUsedHeaders.contains(Alt)) {
        if (DbgLog)
          log("(PCH) file {0} doesn't affect any PCHs", NormFile);
        return;
      }
    }
  }

  {
    std::lock_guard<std::mutex> Lock(ChangedMtx);
    ChangedLastTime = std::chrono::steady_clock::now();
    if (Force) {
      std::string Marked = "*";
      Marked += NormFile;
      Changed.push_back(std::move(Marked));
    } else {
      Changed.push_back(std::move(NormFile));
    }
    ChangedFS = std::move(FS);
  }
  ChangedCV.notify_one();
}

void PCHManager::updateAllHeaders() {
  llvm::StringSet<> NewAllHeaders;
  {
    shared_lck SharedAccessToPCH(PCHLock);
    for (const auto &I : PCHs) {
      for (const auto &H : I->Includes)
        NewAllHeaders.insert(normalizeKey(H));
      NewAllHeaders.insert(normalizeKey(I->Spec.PCHHeader));
    }
  }
  {
    shared_lck DynLock(DynamicPCHLock);
    for (const auto &Entry : DynamicPCHs) {
      for (const auto &H : Entry.second->DynamicIncludes)
        NewAllHeaders.insert(normalizeKey(H.getKey()));
    }
  }

  std::unique_lock<std::shared_timed_mutex> ExclusiveAccess(UsedHeadersLock);
  AllUsedHeaders = std::move(NewAllHeaders);
}

PCHQueue::Task
PCHManager::announcedPCHTask(std::vector<PCHSpec> AnnouncedPCHs) {
  PCHQueue::Task T([this, Specs = std::move(AnnouncedPCHs)]() mutable {
    if (!Initialized.load(std::memory_order_acquire))
      log("(PCH) initializing ({0} announced)", Specs.size());
    analyzePCHDependencies(std::move(Specs));

    if (!Initialized.load(std::memory_order_acquire)) {
      Initialized.store(true, std::memory_order_release);
      InitCV.notify_all();
    }

    unsigned Count = 0;
    {
      shared_lck ReadLock(PCHLock);
      Count = static_cast<unsigned>(PCHs.size());
    }
    rebuildInvalidatedPCH(Count, FSType());
    log("(PCH) updated after announce");
    updateAllHeaders();
  });
  T.ThreadPri = llvm::ThreadPriority::Default;
  return T;
}

void PCHManager::analyzePCHDependencies(std::vector<PCHSpec> PCHSpecs) {
  {
    uniq_lck DynLoc(DynamicPCHLock);
    DynamicPCHs.clear();
  }

  {
    shared_lck ReadAccessToPCH(PCHLock);
    for (auto &I : PCHs)
      I->invalidate();
  }

  uniq_lck ExclusiveAccessToPCH(PCHLock);
  PCHs.clear();
  StaticPCHMap.clear();
  PCHs.reserve(PCHSpecs.size());

  // 1. Partition PCHs with no parent dependency.
  auto NoDepIt = std::partition(PCHSpecs.begin(), PCHSpecs.end(),
                                [](const PCHSpec &S) {
                                  return S.ParentPCHHeader.empty();
                                });
  for (auto J = PCHSpecs.begin(); J != NoDepIt; ++J) {
    auto Item = std::make_shared<PCHItem>(std::move(*J));
    StaticPCHMap[normalizeKey(Item->Spec.PCHHeader)] = Item;
    PCHs.push_back(std::move(Item));
  }

  // 2. Iteratively link dependent levels.
  auto PartBeg = NoDepIt;
  auto End = PCHSpecs.end();
  while (PartBeg != End) {
    auto NewPartEnd =
        std::partition(PartBeg, End, [&](const PCHSpec &S) {
          return StaticPCHMap.contains(normalizeKey(S.ParentPCHHeader));
        });
    if (NewPartEnd == PartBeg) {
      // Unresolved cycle or missing parent: emit remaining as root PCHs to avoid
      // infinite loop.
      for (auto J = PartBeg; J != End; ++J) {
        auto Item = std::make_shared<PCHItem>(std::move(*J));
        StaticPCHMap[normalizeKey(Item->Spec.PCHHeader)] = Item;
        PCHs.push_back(std::move(Item));
      }
      break;
    }

    for (auto J = PartBeg; J != NewPartEnd; ++J) {
      std::string ParentKey = normalizeKey(J->ParentPCHHeader);
      auto Item = std::make_shared<PCHItem>(std::move(*J));
      if (auto DepIt = StaticPCHMap.find(ParentKey);
          DepIt != StaticPCHMap.end()) {
        DepIt->second->DependOnMe.push_back(Item);
        Item->IdependOn.push_back(DepIt->second);
      }
      StaticPCHMap[normalizeKey(Item->Spec.PCHHeader)] = Item;
      PCHs.push_back(std::move(Item));
    }
    PartBeg = NewPartEnd;
  }
}

unsigned PCHManager::PCHItem::invalidate() {
  unsigned Res = 0;
  {
    uniq_lck FullLock(Lock);
    if (ItemState != PCHItem::State::Rebuild) {
      ++Res;
      ItemState = PCHItem::State::Rebuild;
      ++Version;
    }
  }

  if (Res) {
    for (auto &Dep : DependOnMe) {
      if (auto D = Dep.lock())
        Res += D->invalidate();
    }
  }
  return Res;
}

PCHManager::PCHItem::IncFileState::IncFileState(const llvm::vfs::Status &S)
    : Size(S.getSize()),
      ModTime(llvm::sys::toTimePoint(
          llvm::sys::toTimeT(S.getLastModificationTime()))) {}

bool PCHManager::PCHItem::IncFileState::compare(StringRef Path,
                                                const llvm::vfs::Status &S,
                                                bool CheckMD5) {
  if (Size != S.getSize())
    return false;
  auto TModTime =
      llvm::sys::toTimePoint(llvm::sys::toTimeT(S.getLastModificationTime()));
  if (ModTime != TModTime) {
    if (CheckMD5 && MD5 != llvm::MD5::MD5Result{}) {
      if (auto TMD5 = llvm::sys::fs::md5_contents(Path)) {
        if (*TMD5 != MD5) {
          if (DbgLog)
            log("(PCH) MD5 changed for {0}", Path);
          return false;
        }
        ModTime = TModTime;
        return true;
      }
    }
    return false;
  }
  return true;
}

bool PCHManager::PCHItem::isAnyIncludeStateDifferent(FSType &VFS,
                                                     PathRef WorkspaceRoot,
                                                     PathRef SourceRoot) {
  for (auto &[Path, State] : IncludeStates) {
    bool CheckMD5 = isWorkspaceHeader(Path, WorkspaceRoot, SourceRoot);
    auto Status = VFS->status(Path);
    if (!Status &&
        llvm::sys::path::is_style_windows(llvm::sys::path::Style::native)) {
      std::string Alt = normalizePathDriveLetter(Path, true);
      Status = VFS->status(Alt);
      if (!Status) {
        Alt = normalizePathDriveLetter(Path, false);
        Status = VFS->status(Alt);
      }
    }
    if (Status) {
      if (!State.compare(Path, *Status, CheckMD5)) {
        if (DbgLog)
          elog("(PCH) Include state changed for {0}", Path);
        return true;
      }
    }
  }
  return false;
}

bool PCHManager::PCHItem::isIncludeStateDifferent(StringRef Path, FSType &VFS,
                                                  bool CheckMD5) {
  auto It = IncludeStates.find(Path);
  if (It == IncludeStates.end())
    return false;
  auto Status = VFS->status(Path);
  if (!Status &&
      llvm::sys::path::is_style_windows(llvm::sys::path::Style::native)) {
    std::string Alt = normalizePathDriveLetter(Path, true);
    Status = VFS->status(Alt);
    if (!Status) {
      Alt = normalizePathDriveLetter(Path, false);
      Status = VFS->status(Alt);
    }
  }
  if (Status)
    return !It->getValue().compare(Path, *Status, CheckMD5);
  return true;
}

void PCHManager::PCHItem::updateIncludeStates(FSType &VFS,
                                              PathRef WorkspaceRoot,
                                              PathRef SourceRoot) {
  IncludeStates.clear();
  auto Record = [&](StringRef IncPath) {
    if (auto S = VFS->status(IncPath)) {
      auto [It, Inserted] = IncludeStates.insert_or_assign(IncPath, *S);
      if (isWorkspaceHeader(IncPath, WorkspaceRoot, SourceRoot)) {
        if (auto Hash = llvm::sys::fs::md5_contents(IncPath))
          It->second.MD5 = *Hash;
        else {
          (void)Hash.getError();
          It->second.MD5 = {};
        }
      } else {
        It->second.MD5 = {};
      }
    }
  };

  for (const auto &I : Includes)
    Record(I);
  for (const auto &H : DynamicIncludes)
    Record(H.getKey());
}

unsigned
PCHManager::invalidateAffectedPCH(const std::vector<std::string> &ChangedFiles,
                                  FSType FS) {
  unsigned Invalidated = 0;
  llvm::StringSet<> ChangedSet;
  llvm::StringSet<> ForcedSet;
  for (const std::string &Raw : ChangedFiles) {
    bool Forced = !Raw.empty() && Raw[0] == '*';
    std::string Norm = normalizeKey(Forced ? StringRef(Raw).drop_front(1)
                                           : StringRef(Raw));
    ChangedSet.insert(Norm);
    if (Forced)
      ForcedSet.insert(Norm);
  }

  std::string WRoot = WorkspaceRoot.value_or("");
  {
    shared_lck SharedPCH(PCHLock);
    for (auto &UI : PCHs) {
      PCHItem &Item = *UI;
      if (Item.ItemState == PCHItem::State::Rebuild)
        continue;

      std::string SRoot;
      if (auto PI = CDB.getProjectInfo(Item.Spec.PCHHeader))
        SRoot = PI->SourceRoot;

      if (ChangedSet.contains(normalizeKey(Item.Spec.PCHHeader))) {
        Invalidated += Item.invalidate();
        continue;
      }

      for (const std::string &S : Item.Includes) {
        std::string NormInc = normalizeKey(S);
        if (!ChangedSet.contains(NormInc))
          continue;
        bool Forced = ForcedSet.contains(NormInc);
        if (Forced ||
            !FS ||
            Item.isIncludeStateDifferent(
                S, FS, isWorkspaceHeader(S, WRoot, SRoot))) {
          Invalidated += Item.invalidate();
          break;
        }
      }
    }
  }

  {
    shared_lck DynLock(DynamicPCHLock);
    for (auto &Entry : DynamicPCHs) {
      PCHItem &Item = *Entry.second;
      if (Item.ItemState == PCHItem::State::Rebuild)
        continue;

      if (!Item.IdependOn.empty() &&
          Item.IdependOn[0]->ItemState == PCHItem::State::Rebuild) {
        Invalidated += Item.invalidate();
        continue;
      }

      std::string SRoot;
      if (auto PI = CDB.getProjectInfo(Item.Spec.PCHHeader))
        SRoot = PI->SourceRoot;

      for (const auto &H : Item.DynamicIncludes) {
        std::string NormInc = normalizeKey(H.getKey());
        if (!ChangedSet.contains(NormInc))
          continue;
        bool Forced = ForcedSet.contains(NormInc);
        if (Forced ||
            !FS ||
            Item.isIncludeStateDifferent(
                H.getKey(), FS,
                isWorkspaceHeader(H.getKey(), WRoot, SRoot))) {
          Invalidated += Item.invalidate();
          break;
        }
      }
    }
  }
  return Invalidated;
}

std::optional<PCHManager::PCHItem::IncFileState>
PCHManager::PCHItem::IncFileState::read(StringRef &Data) {
  constexpr size_t Needed = 8 + 8 + sizeof(MD5);
  if (Data.size() < Needed)
    return std::nullopt;
  IncFileState Res;
  Res.Size = llvm::support::endian::read64le(Data.data());
  Data = Data.drop_front(8);
  uint64_t UnixTime = llvm::support::endian::read64le(Data.data());
  Data = Data.drop_front(8);
  Res.ModTime = llvm::sys::toTimePoint(UnixTime);
  std::memcpy(Res.MD5.data(), Data.data(), sizeof(Res.MD5));
  Data = Data.drop_front(sizeof(Res.MD5));
  return Res;
}

void PCHManager::PCHItem::IncFileState::write(llvm::raw_ostream &OS) const {
  uint64_t Sz = Size;
  uint64_t UnixEpoch = llvm::sys::toTimeT(ModTime);
  char Buf[16];
  llvm::support::endian::write64le(Buf, Sz);
  llvm::support::endian::write64le(Buf + 8, UnixEpoch);
  OS.write(Buf, sizeof(Buf));
  OS.write(reinterpret_cast<const char *>(MD5.data()), sizeof(MD5));
}

bool PCHManager::tryAddDynamicPCH(PathRef File, FSType FS) {
  auto Spec = CDB.getPCHSpec(File);
  if (!Spec || !Spec->IsDynamic)
    return false;

  std::string Key = normalizeKey(File);
  {
    uniq_lck Lock(DynamicPCHLock);
    if (auto It = DynamicPCHs.find(Key); It != DynamicPCHs.end()) {
      It->second->LastUsedSeq = ++DynamicUseSeq;
      return true;
    }
  }

  shared_pch_item ParentItem;
  if (!Spec->ParentPCHHeader.empty()) {
    shared_lck Lock(PCHLock);
    if (auto It = StaticPCHMap.find(normalizeKey(Spec->ParentPCHHeader));
        It != StaticPCHMap.end())
      ParentItem = It->second;
  }

  {
    uniq_lck Lock(DynamicPCHLock);
    if (auto It = DynamicPCHs.find(Key); It != DynamicPCHs.end()) {
      It->second->LastUsedSeq = ++DynamicUseSeq;
      return true;
    }

    auto Item = std::make_shared<PCHItem>(std::move(*Spec));
    if (ParentItem) {
      Item->IdependOn.push_back(ParentItem);
      ParentItem->DependOnMe.push_back(Item);
    }
    Item->Dynamic = true;
    Item->LastUsedSeq = ++DynamicUseSeq;
    DynamicPCHs[Key] = Item;

    if (DbgLog)
      log("(DynPCH) added dynamic PCH for {0} (parent: {1})", File,
          ParentItem ? ParentItem->Spec.PCHHeader : "<none>");
  }

  evictDynamicPCHsIfNeeded();
  Queue.push(PCHQueue::Task(
      [this, FS = std::move(FS)] { rebuildInvalidatedPCH(1, FS); }));
  return true;
}

bool PCHManager::tryRemoveDynamicPCH(PathRef File) {
  std::string Key = normalizeKey(File);
  uniq_lck Lock(DynamicPCHLock);
  if (DynamicPCHs.erase(Key)) {
    if (DbgLog)
      log("(DynPCH) removed dynamic PCH for {0}", File);
    return true;
  }
  return false;
}

void PCHManager::evictDynamicPCHsIfNeeded() {
  if (MaxDynamicPCHs == 0)
    return;
  uniq_lck Lock(DynamicPCHLock);
  while (DynamicPCHs.size() > MaxDynamicPCHs) {
    auto OldestIt = DynamicPCHs.end();
    uint64_t MinSeq = std::numeric_limits<uint64_t>::max();
    for (auto It = DynamicPCHs.begin(); It != DynamicPCHs.end(); ++It) {
      if (It->second->LastUsedSeq < MinSeq) {
        MinSeq = It->second->LastUsedSeq;
        OldestIt = It;
      }
    }
    if (OldestIt == DynamicPCHs.end())
      break;
    if (DbgLog)
      log("(DynPCH) evicting LRU dynamic PCH {0}", OldestIt->first());
    DynamicPCHs.erase(OldestIt);
  }
}

void PCHManager::addDynamicGhost(
    const shared_pch_item &Item,
    IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> MemFS) {
  auto Buf = llvm::MemoryBuffer::getMemBuffer("\nnamespace{};\n");
  MemFS->addFile(Item->Spec.BuildCommand.Filename, 0, std::move(Buf));
}

void PCHManager::makeSnapshot(const shared_pch_item &Item,
                              const PCHSnapshotPtr &Snap) {
  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> PCHFS(
      new llvm::vfs::InMemoryFileSystem());
  Snap->MemFS = PCHFS;
  if (Item->Dynamic)
    addDynamicGhost(Item, PCHFS);

  if (Snap->PCHData) {
    PCHFS->addFile(Snap->Filename + ".pch", 0,
                   llvm::MemoryBuffer::getMemBuffer(
                       Snap->PCHData->getBuffer(), "", false));
  }

  size_t N = std::min(Snap->PCHItems.size(), Snap->UsedPCHDatasSnapshot.size());
  for (size_t I = 0; I < N; ++I) {
    const auto &D = Snap->PCHItems[I];
    const auto &B = Snap->UsedPCHDatasSnapshot[I];
    if (B) {
      PCHFS->addFile(D->Spec.PCHHeader + ".pch", 0,
                     llvm::MemoryBuffer::getMemBuffer(B->getBuffer(), "", false));
    }
  }
}

uint64_t PCHManager::computeCommandHash(const tooling::CompileCommand &Cmd) {
  std::string Joined = Cmd.Directory;
  for (const std::string &Arg : Cmd.CommandLine) {
    Joined.push_back('\0');
    Joined += Arg;
  }
  return llvm::xxh3_64bits(
      reinterpret_cast<const uint8_t *>(Joined.data()), Joined.size());
}

PPSkipIncludes::PPSkipIncludes(SourceManager &SM, StringRef TargetFile,
                               bool SkipTarget)
    : Target(TargetFile.str()), TargetNorm(normalizeKey(TargetFile)),
      SkipTarget(SkipTarget) {
  if (auto E = SM.getFileManager().getFileRef(Target))
    TargetRef = *E;
}

bool PPSkipIncludes::InclusionAllowed(
    SourceLocation HashLoc, const Token &IncludeTok, StringRef FileName,
    bool IsAngled, CharSourceRange FilenameRange, OptionalFileEntryRef File,
    StringRef SearchPath, StringRef RelativePath, const Module *Imported,
    SrcMgr::CharacteristicKind FileType) {
  if (Skipped)
    return false;

  if (File) {
    bool IsTarget = TargetRef && (*TargetRef == *File);
    if (!IsTarget) {
      StringRef Real = File->getFileEntry().tryGetRealPathName();
      if (!Real.empty() && pathEqual(normalizeKey(Real), TargetNorm))
        IsTarget = true;
    }
    if (IsTarget) {
      Skipped = true;
      if (!SkipTarget)
        AllowedIncludes.insert(
            File->getFileEntry().tryGetRealPathName().str());
      return !SkipTarget;
    }
    AllowedIncludes.insert(File->getFileEntry().tryGetRealPathName().str());
  }
  return true;
}

void PCHManager::rebuildPCH(shared_pch_item ShItem, FSType FS) {
  auto &Item = *ShItem;
  auto S = PCHItem::State::Invalid;
  llvm::scope_exit OnExit([&] {
    if (OnProgress)
      OnProgress(Stats{++Complete, Total});
    {
      // Ensure state is published while holding nothing extra, then notify.
      Item.ItemState = S;
    }
    Item.CV.notify_all();
    OnPCHBuilt.broadcast(
        PCHEvent{Item.Spec.PCHHeader, S == PCHItem::State::Valid});
  });

  uniq_lck ThisItemFullLock(Item.Lock);
  shared_pch_item Dep;
  if (!Item.IdependOn.empty())
    Dep = Item.IdependOn[0];

  shared_lck DepReadLock = Dep ? shared_lck(Dep->Lock) : shared_lck{};
  if (Dep && Dep->ItemState == PCHItem::State::Rebuild) {
    if (DbgLog)
      log("(PCH) {0} waiting for parent PCH {1}", Item.Spec.PCHHeader,
          Dep->Spec.PCHHeader);
    Dep->CV.wait(DepReadLock,
                 [&] { return Dep->ItemState != PCHItem::State::Rebuild; });
  }
  if (Dep && Dep->ItemState != PCHItem::State::Valid) {
    elog("(PCH) Cannot rebuild PCH for {0} because parent {1} is invalid",
         Item.Spec.PCHHeader, Dep->Spec.PCHHeader);
    return;
  }

  PCHSnapshotPtr DepSnapshot;
  PCHSnapshotPtr NewSnapshot = std::make_shared<PCHSnapshot>();
  NewSnapshot->Filename = Item.Spec.PCHHeader;
  if (Dep) {
    DepSnapshot = std::atomic_load(&Dep->PCHDatasSnapshot);
    if (!DepSnapshot) {
      elog("(PCH) Parent {0} has null snapshot for {1}", Dep->Spec.PCHHeader,
           Item.Spec.PCHHeader);
      return;
    }
    NewSnapshot->PCHItems = DepSnapshot->PCHItems;
    NewSnapshot->PCHItems.push_back(Dep);
    NewSnapshot->UsedPCHDatasSnapshot = DepSnapshot->UsedPCHDatasSnapshot;
    NewSnapshot->UsedPCHDatasSnapshot.push_back(DepSnapshot->PCHData);
  }

  tooling::CompileCommand BuildCC = Item.Spec.BuildCommand;
  if (!Item.Dynamic) {
    if (auto Mangled = CDB.getCompileCommand(Item.Spec.PCHHeader))
      BuildCC = std::move(*Mangled);
  } else {
    if (auto MangledSpec = CDB.getPCHSpec(Item.Spec.PCHHeader))
      BuildCC = std::move(MangledSpec->BuildCommand);
  }
  uint64_t CmdHash = computeCommandHash(BuildCC);

  std::string WRoot = WorkspaceRoot.value_or("");
  std::string SRoot;
  if (auto PI = CDB.getProjectInfo(Item.Spec.PCHHeader))
    SRoot = PI->SourceRoot;

  // Static PCHs only: try loading from disk cache. Dynamic ghost PCHs stay
  // purely in-memory.
  llvm::SmallString<128> PCHCachePath;
  if (!Item.Dynamic) {
    if (auto PCHCacheDir = getPCHCacheDirFor(NewSnapshot->Filename)) {
      llvm::StringRef DirRef = *PCHCacheDir;
      llvm::StringRef ItemFN = NewSnapshot->Filename;
      while (!ItemFN.starts_with(DirRef) && !DirRef.empty())
        DirRef = llvm::sys::path::parent_path(DirRef);

      llvm::SmallString<128> CacheName(ItemFN.drop_front(DirRef.size()));
      CacheName += ".pch_cache";
      std::replace(CacheName.begin(), CacheName.end(), '/', '_');
      std::replace(CacheName.begin(), CacheName.end(), '\\', '_');
      std::replace(CacheName.begin(), CacheName.end(), ':', '_');

      PCHCachePath = *PCHCacheDir;
      llvm::sys::path::append(PCHCachePath, CacheName);
    }
  }

  auto OrigV = Item.Version;
  if (!Item.Dynamic && !std::atomic_load(&Item.PCHDatasSnapshot) &&
      !PCHCachePath.empty() && llvm::sys::fs::exists(PCHCachePath)) {
    if (auto Buffer = llvm::MemoryBuffer::getFile(PCHCachePath)) {
      StringRef Data = (*Buffer)->getBuffer();
      PCHItem::DependencyVersions DepsV;
      if (PCHItem::read(Data, Item, DepsV, CmdHash)) {
        auto VFS = TFS.view(BuildCC.Directory);
        bool Diff = DepsV.size() != Item.IdependOn.size();
        if (!Diff && Item.isAnyIncludeStateDifferent(VFS, WRoot, SRoot))
          Diff = true;
        if (!Diff) {
          for (size_t I = 0; I < DepsV.size(); ++I) {
            const auto &D = Item.IdependOn[I];
            if (D->Spec.PCHHeader != DepsV[I].FName ||
                static_cast<uint32_t>(D->Version) != DepsV[I].Version) {
              Diff = true;
              break;
            }
          }
        }
        if (!Diff) {
          NewSnapshot->PCHData = Item.PCHData;
          NewSnapshot->Version = Item.Version;
          makeSnapshot(ShItem, NewSnapshot);
          std::atomic_store(&Item.PCHDatasSnapshot, NewSnapshot);
          S = PCHItem::State::Valid;
          if (DbgLog)
            log("(PCH) Loaded cached PCH ({0} bytes) for {1}",
                Item.PCHData ? Item.PCHData->getBufferSize() : 0,
                Item.Spec.PCHHeader);
          return;
        }
        if (DbgLog)
          log("(PCH) Cache invalid for {0}; rebuilding", Item.Spec.PCHHeader);
        ++Item.Version;
        if (Item.Version == OrigV)
          ++Item.Version;
      }
    }
  }

  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.ForceRebuild = true;
  Inputs.CompileCommand = BuildCC;

  if (DbgLog) {
    std::string CmdStr = std::accumulate(
        BuildCC.CommandLine.begin(), BuildCC.CommandLine.end(), std::string(),
        [](std::string R, const std::string &Arg) { return R + " " + Arg; });
    log("(PCH) cmdline for {0}:{1}", Item.Spec.PCHHeader, CmdStr);
  }

  StoreDiags CompilerInvocationDiagConsumer;
  std::vector<std::string> CC1Args;
  std::shared_ptr<CompilerInvocation> Invocation =
      buildCompilerInvocation(Inputs, CompilerInvocationDiagConsumer, &CC1Args);
  if (!Invocation) {
    elog("(PCH) Could not build CompilerInvocation for {0}",
         Item.Spec.PCHHeader);
    return;
  }

  auto &PreprocessorOpts = Invocation->getPreprocessorOpts();
  PreprocessorOpts.PrecompiledPreambleBytes.first = 0;
  PreprocessorOpts.PrecompiledPreambleBytes.second = false;
  PreprocessorOpts.DisablePCHOrModuleValidation =
      DisableValidationForModuleKind::PCH;
  PreprocessorOpts.WriteCommentListToPCH = false;

  auto VFS = TFS.view(BuildCC.Directory);
  if (FS) {
    IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
        new llvm::vfs::OverlayFileSystem(VFS));
    Overlay->pushOverlay(FS);
    VFS = Overlay;
  }

  if (Item.Dynamic) {
    IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> DynamicFS(
        new llvm::vfs::InMemoryFileSystem());
    addDynamicGhost(ShItem, DynamicFS);
    IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
        new llvm::vfs::OverlayFileSystem(VFS));
    Overlay->pushOverlay(DynamicFS);
    VFS = Overlay;
  }

  if (DepSnapshot) {
    PreprocessorOpts.ImplicitPCHInclude = Dep->Spec.PCHHeader + ".pch";
    IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
        new llvm::vfs::OverlayFileSystem(VFS));
    Overlay->pushOverlay(DepSnapshot->MemFS);
    VFS = Overlay;
  }

  CppFilePreambleCallbacks SerializedDeclsCollector(
      Item.Spec.PCHHeader,
      [&](CapturedASTCtx AST,
          std::shared_ptr<const include_cleaner::PragmaIncludes> PragmaIncs) {
        if (!Item.Dynamic) {
          Callbacks.onPreambleAST(Item.Spec.PCHHeader,
                                  std::to_string(Item.Version), std::move(AST),
                                  std::move(PragmaIncs));
        }
      });

  FrontendOptions &FrontendOpts = Invocation->getFrontendOpts();
  FrontendOpts.ProgramAction = frontend::GeneratePCH;
  FrontendOpts.SkipFunctionBodies = true;

  StoreDiags PreambleDiagnostics;
  PreambleDiagnostics.setDiagCallback(
      [&Item](const clang::Diagnostic &, clangd::Diag &Diag) {
        if (Diag.Severity >= DiagnosticsEngine::Level::Error) {
          elog("(PCH) Error building {0}: {1}", Item.Spec.PCHHeader,
               Diag.Message);
        }
      });
  llvm::IntrusiveRefCntPtr<DiagnosticsEngine> PreambleDiagsEngine =
      CompilerInstance::createDiagnostics(*VFS, Invocation->getDiagnosticOpts(),
                                          &PreambleDiagnostics, false, nullptr);

  std::unique_ptr<CompilerInstance> Clang(new CompilerInstance(
      Invocation, std::make_shared<PCHContainerOperations>(), nullptr));
  llvm::CrashRecoveryContextCleanupRegistrar<CompilerInstance> CICleanup(
      Clang.get());

  Clang->createVirtualFileSystem(VFS, &PreambleDiagnostics);
  Clang->setDiagnostics(&*PreambleDiagsEngine);
  if (!Clang->createTarget()) {
    elog("(PCH) Failed to create target for {0}", Item.Spec.PCHHeader);
    return;
  }
  if (Clang->getFrontendOpts().Inputs.size() != 1 ||
      Clang->getFrontendOpts().Inputs[0].getKind().getFormat() !=
          InputKind::Source ||
      Clang->getFrontendOpts().Inputs[0].getKind().getLanguage() ==
          Language::LLVM_IR) {
    elog("(PCH) Bad inputs for {0}", Item.Spec.PCHHeader);
    return;
  }

  Clang->createFileManager();
  Clang->setSourceManager(
      new SourceManager(*PreambleDiagsEngine, Clang->getFileManager()));

  Clang->getLangOpts().CompilingPCH = true;
  Clang->createPreprocessor(TU_Prefix);

  SmallVector<char, 0> RawPCHBytes;
  auto Act = std::make_unique<PrecompilePCHAction>(&RawPCHBytes,
                                                   SerializedDeclsCollector);
  SerializedDeclsCollector.BeforeExecute(*Clang);
  if (!Act->BeginSourceFile(*Clang, Clang->getFrontendOpts().Inputs[0])) {
    elog("(PCH) Failed to begin source file {0}", Item.Spec.PCHHeader);
    return;
  }
  SerializedDeclsCollector.InitiateIncludeCollection(*Clang);

  if (auto Delegated = SerializedDeclsCollector.createPPCallbacks())
    Clang->getPreprocessor().addPPCallbacks(std::move(Delegated));

  PPSkipIncludes *SkipCB = nullptr;
  if (Item.Dynamic && !Item.Spec.StopBeforeHeader.empty()) {
    Clang->getPreprocessor().addPPCallbacks(
        std::unique_ptr<PPCallbacks>(
            SkipCB = new PPSkipIncludes(Clang->getSourceManager(),
                                        Item.Spec.StopBeforeHeader,
                                        /*SkipTarget=*/true)));
  }

  if (auto *CommentHandler = SerializedDeclsCollector.getCommentHandler())
    Clang->getPreprocessor().addCommentHandler(CommentHandler);

  if (llvm::Error Err = Act->Execute()) {
    elog("(PCH) Failure executing clang for {0}: {1}", Item.Spec.PCHHeader,
         llvm::toString(std::move(Err)));
    return;
  }

  SerializedDeclsCollector.AfterExecute(*Clang);
  Act->EndSourceFile();

  if (!Act->hasEmittedPreamblePCH()) {
    elog("(PCH) Failed to emit PCH for {0} (Version {1})", Item.Spec.PCHHeader,
         Item.Version);
    return;
  }

  if (SkipCB && !SkipCB->wasSkipped()) {
    elog("(PCH) Expected to skip {0} while generating dynamic PCH for {1}",
         SkipCB->getTarget(), Item.Spec.PCHHeader);
  }

  Item.Includes = SerializedDeclsCollector.takeIncludes().takeAllHeaders();
  if (Item.Dynamic && SkipCB)
    Item.DynamicIncludes = SkipCB->takeAllowedIncludes();
  Item.updateIncludeStates(VFS, WRoot, SRoot);

  auto NewPCHBuf = llvm::MemoryBuffer::getMemBufferCopy(
      StringRef(RawPCHBytes.data(), RawPCHBytes.size()));
  Item.PCHData = std::shared_ptr<const llvm::MemoryBuffer>(std::move(NewPCHBuf));

  NewSnapshot->PCHData = Item.PCHData;
  NewSnapshot->Version = Item.Version;
  makeSnapshot(ShItem, NewSnapshot);
  std::atomic_store(&Item.PCHDatasSnapshot, NewSnapshot);
  S = PCHItem::State::Valid;

  log("(PCH) Built {0}PCH of size {1} bytes for {2} (Version: {3})",
      Item.Dynamic ? "dynamic " : "", Item.PCHData->getBufferSize(),
      Item.Spec.PCHHeader, Item.Version);

  if (!Item.Dynamic && !PCHCachePath.empty()) {
    std::string Serialized;
    llvm::raw_string_ostream OS(Serialized);
    if (PCHItem::storeTo(OS, Item, CmdHash)) {
      std::string CacheDest = std::string(PCHCachePath);
      CacheWritePool.runAsync(
          "pch-cache-write",
          [CacheDest = std::move(CacheDest),
           Payload = std::move(Serialized)]() {
            llvm::sys::fs::create_directories(
                llvm::sys::path::parent_path(CacheDest));
            std::string TmpPath =
                CacheDest + ".tmp." +
                std::to_string(llvm::sys::Process::getProcessId());
            if (auto Err = llvm::writeToOutput(
                    TmpPath, [&](llvm::raw_ostream &Out) {
                      Out.write(Payload.data(), Payload.size());
                      return llvm::Error::success();
                    })) {
              elog("(PCH) Failed writing cache {0}: {1}", TmpPath,
                   llvm::toString(std::move(Err)));
              llvm::sys::fs::remove(TmpPath);
              return;
            }
            if (auto EC = llvm::sys::fs::rename(TmpPath, CacheDest)) {
              elog("(PCH) Failed renaming cache {0} -> {1}: {2}", TmpPath,
                   CacheDest, EC.message());
              llvm::sys::fs::remove(TmpPath);
            }
          });
    }
  }
}

bool PCHManager::PCHItem::storeTo(llvm::raw_ostream &OS, const PCHItem &I,
                                  uint64_t CommandHash) {
  auto WriteU32 = [&OS](uint32_t V) {
    char Buf[4];
    llvm::support::endian::write32le(Buf, V);
    OS.write(Buf, 4);
  };
  auto WriteU64 = [&OS](uint64_t V) {
    char Buf[8];
    llvm::support::endian::write64le(Buf, V);
    OS.write(Buf, 8);
  };
  auto WriteStr = [&](StringRef S) {
    WriteU32(static_cast<uint32_t>(S.size()));
    OS.write(S.data(), S.size());
  };

  OS.write(kPCHCacheMagic, sizeof(kPCHCacheMagic));
  WriteU32(kPCHCacheSchemaVersion);
  WriteStr(clang::getClangFullVersion());
  WriteU64(CommandHash);

  WriteU32(static_cast<uint32_t>(I.Version));

  WriteU32(static_cast<uint32_t>(I.Includes.size()));
  for (const auto &V : I.Includes)
    WriteStr(V);

  WriteU32(static_cast<uint32_t>(I.DynamicIncludes.size()));
  for (const auto &V : I.DynamicIncludes)
    WriteStr(V.first());

  WriteU32(static_cast<uint32_t>(I.IncludeStates.size()));
  for (const auto &[K, V] : I.IncludeStates) {
    WriteStr(K);
    V.write(OS);
  }

  WriteU32(static_cast<uint32_t>(I.IdependOn.size()));
  for (const auto &D : I.IdependOn) {
    WriteStr(D->Spec.PCHHeader);
    WriteU32(static_cast<uint32_t>(D->Version));
  }

  if (I.PCHData)
    WriteStr(I.PCHData->getBuffer());
  else
    WriteU32(0);

  OS.flush();
  return true;
}

bool PCHManager::PCHItem::read(StringRef &Data, PCHItem &I,
                               DependencyVersions &Deps,
                               uint64_t ExpectedCmdHash) {
  auto ReadU32 = [&](uint32_t &Out) -> bool {
    if (Data.size() < 4)
      return false;
    Out = llvm::support::endian::read32le(Data.data());
    Data = Data.drop_front(4);
    return true;
  };
  auto ReadU64 = [&](uint64_t &Out) -> bool {
    if (Data.size() < 8)
      return false;
    Out = llvm::support::endian::read64le(Data.data());
    Data = Data.drop_front(8);
    return true;
  };
  auto ReadStr = [&](std::string &Out) -> bool {
    uint32_t Len = 0;
    if (!ReadU32(Len) || Data.size() < Len)
      return false;
    Out.assign(Data.data(), Len);
    Data = Data.drop_front(Len);
    return true;
  };
  auto ReadStrRef = [&](StringRef &Out) -> bool {
    uint32_t Len = 0;
    if (!ReadU32(Len) || Data.size() < Len)
      return false;
    Out = Data.take_front(Len);
    Data = Data.drop_front(Len);
    return true;
  };

  if (Data.size() < sizeof(kPCHCacheMagic) ||
      std::memcmp(Data.data(), kPCHCacheMagic, sizeof(kPCHCacheMagic)) != 0)
    return false;
  Data = Data.drop_front(sizeof(kPCHCacheMagic));

  uint32_t Schema = 0;
  if (!ReadU32(Schema) || Schema != kPCHCacheSchemaVersion)
    return false;

  std::string ClangVer;
  if (!ReadStr(ClangVer) || ClangVer != clang::getClangFullVersion())
    return false;

  uint64_t CmdHash = 0;
  if (!ReadU64(CmdHash) || CmdHash != ExpectedCmdHash)
    return false;

  uint32_t Ver = 0;
  if (!ReadU32(Ver))
    return false;
  I.Version = static_cast<int>(Ver);

  uint32_t IncCount = 0;
  if (!ReadU32(IncCount))
    return false;
  I.Includes.resize(IncCount);
  for (uint32_t J = 0; J < IncCount; ++J) {
    if (!ReadStr(I.Includes[J]))
      return false;
  }

  uint32_t DynIncCount = 0;
  if (!ReadU32(DynIncCount))
    return false;
  I.DynamicIncludes.clear();
  for (uint32_t J = 0; J < DynIncCount; ++J) {
    std::string S;
    if (!ReadStr(S))
      return false;
    I.DynamicIncludes.insert(S);
  }

  uint32_t StateCount = 0;
  if (!ReadU32(StateCount))
    return false;
  I.IncludeStates.clear();
  for (uint32_t J = 0; J < StateCount; ++J) {
    std::string K;
    if (!ReadStr(K))
      return false;
    auto St = IncFileState::read(Data);
    if (!St)
      return false;
    I.IncludeStates[K] = *St;
  }

  uint32_t DepCount = 0;
  if (!ReadU32(DepCount))
    return false;
  Deps.resize(DepCount);
  for (uint32_t J = 0; J < DepCount; ++J) {
    if (!ReadStr(Deps[J].FName) || !ReadU32(Deps[J].Version))
      return false;
  }

  StringRef PCHPayload;
  if (!ReadStrRef(PCHPayload))
    return false;
  auto Buf = llvm::MemoryBuffer::getMemBufferCopy(PCHPayload);
  I.PCHData = std::shared_ptr<const llvm::MemoryBuffer>(std::move(Buf));
  return true;
}

void PCHManager::rebuildInvalidatedPCH(unsigned TotalCount, FSType FS) {
  Complete = 0;
  Total = TotalCount;
  if (OnProgress)
    OnProgress(Stats{Complete, Total});

  std::vector<shared_pch_item> ToRebuild;
  {
    shared_lck Lock(PCHLock);
    for (const auto &I : PCHs) {
      if (I->ItemState == PCHItem::State::Rebuild)
        ToRebuild.push_back(I);
    }
  }
  {
    shared_lck DynLock(DynamicPCHLock);
    for (const auto &Entry : DynamicPCHs) {
      if (Entry.second->ItemState == PCHItem::State::Rebuild)
        ToRebuild.push_back(Entry.second);
    }
  }

  if (ToRebuild.empty())
    return;

  unsigned NumWorkers =
      std::min<unsigned>(static_cast<unsigned>(ToRebuild.size()),
                         std::max(1u, MaxConcurrency));
  std::atomic<size_t> NextIdx{0};
  std::vector<std::thread> Workers;
  Workers.reserve(NumWorkers);
  for (unsigned W = 0; W < NumWorkers; ++W) {
    Workers.emplace_back([this, &ToRebuild, &NextIdx, FS]() {
      while (true) {
        size_t Idx = NextIdx.fetch_add(1, std::memory_order_relaxed);
        if (Idx >= ToRebuild.size())
          break;
        rebuildPCH(ToRebuild[Idx], FS);
      }
    });
  }
  for (auto &T : Workers)
    T.join();
}

PCHManager::PCHAccess
PCHManager::tryFindPCH(PathRef File, const tooling::CompileCommand &Cmd) const {
  if (!Initialized.load(std::memory_order_acquire) && !WaitForInit) {
    if (DbgLog)
      log("(tryFindPCH) not initialized yet; returning empty for {0}", File);
    return {};
  }
  return findPCH(File, Cmd);
}

PCHManager::PCHAccess PCHManager::tryFindPCH(PathRef PCHFile) const {
  if (!Initialized.load(std::memory_order_acquire) && !WaitForInit) {
    if (DbgLog)
      log("(tryFindPCH) not initialized yet; returning empty for {0}", PCHFile);
    return {};
  }
  return findPCH(PCHFile);
}

bool PCHManager::hasPCHInDependencies(PathRef File,
                                      const tooling::CompileCommand &Cmd,
                                      PathRef PCHFile) const {
  auto Spec = CDB.getPCHSpec(File);
  if (!Spec)
    return false;

  std::string TargetKey = normalizeKey(PCHFile);
  if (Spec->IsDynamic) {
    shared_lck Lock(DynamicPCHLock);
    if (auto It = DynamicPCHs.find(normalizeKey(File));
        It != DynamicPCHs.end()) {
      const PCHItem *Cur = It->second.get();
      while (Cur) {
        if (pathEqual(normalizeKey(Cur->Spec.PCHHeader), TargetKey))
          return true;
        Cur = Cur->IdependOn.empty() ? nullptr : Cur->IdependOn[0].get();
      }
    }
    return false;
  }

  shared_lck Lock(PCHLock);
  if (auto It = StaticPCHMap.find(normalizeKey(Spec->PCHHeader));
      It != StaticPCHMap.end()) {
    const PCHItem *Cur = It->second.get();
    while (Cur) {
      if (pathEqual(normalizeKey(Cur->Spec.PCHHeader), TargetKey))
        return true;
      Cur = Cur->IdependOn.empty() ? nullptr : Cur->IdependOn[0].get();
    }
  }
  return false;
}

PCHManager::PCHAccess
PCHManager::findPCH(PathRef File, const tooling::CompileCommand &) const {
  auto Spec = CDB.getPCHSpec(File);
  if (!Spec)
    return {};
  if (Spec->IsDynamic)
    return findDynPCH(Spec->PCHHeader);
  return findPCH(Spec->PCHHeader);
}

PCHManager::PCHAccess PCHManager::findDynPCH(PathRef PCHFile) const {
  if (!Initialized.load(std::memory_order_acquire)) {
    shared_lck Lock(PCHLock);
    InitCV.wait(Lock,
                [&] { return Initialized.load(std::memory_order_acquire); });
  }

  shared_pch_item Res;
  std::string Key = normalizeKey(PCHFile);
  {
    shared_lck Lock(DynamicPCHLock);
    if (auto It = DynamicPCHs.find(Key); It != DynamicPCHs.end())
      Res = It->second;
  }
  if (!Res)
    return {};

  Res->LastUsedSeq = ++DynamicUseSeq;
  PCHSnapshotPtr Snap = std::atomic_load(&Res->PCHDatasSnapshot);
  if (!Snap) {
    shared_lck ItemLock(Res->Lock);
    if (Res->ItemState == PCHItem::State::Rebuild) {
      Res->CV.wait(ItemLock,
                   [&] { return Res->ItemState != PCHItem::State::Rebuild; });
    }
    if (Res->ItemState != PCHItem::State::Valid)
      return {};
    Snap = std::atomic_load(&Res->PCHDatasSnapshot);
  }
  if (!Snap)
    return {};
  return PCHAccess(std::move(Snap), std::move(Res),
                   const_cast<PCHManager *>(this));
}

PCHManager::PCHAccess PCHManager::findPCH(PathRef PCHFile) const {
  if (!Initialized.load(std::memory_order_acquire)) {
    shared_lck Lock(PCHLock);
    InitCV.wait(Lock,
                [&] { return Initialized.load(std::memory_order_acquire); });
  }

  shared_pch_item Res;
  std::string Key = normalizeKey(PCHFile);
  {
    shared_lck Lock(PCHLock);
    if (auto It = StaticPCHMap.find(Key); It != StaticPCHMap.end())
      Res = It->second;
  }
  if (!Res)
    return {};

  PCHSnapshotPtr Snap = std::atomic_load(&Res->PCHDatasSnapshot);
  if (!Snap) {
    shared_lck ItemLock(Res->Lock);
    if (Res->ItemState == PCHItem::State::Rebuild) {
      Res->CV.wait(ItemLock,
                   [&] { return Res->ItemState != PCHItem::State::Rebuild; });
    }
    if (Res->ItemState != PCHItem::State::Valid)
      return {};
    Snap = std::atomic_load(&Res->PCHDatasSnapshot);
  }
  if (!Snap)
    return {};
  return PCHAccess(std::move(Snap), std::move(Res),
                   const_cast<PCHManager *>(this));
}

bool PCHManager::PCHAccess::addPCH(CompilerInvocation *CI, FSType &VFS) const {
  if (!ItemSnapshot)
    return false;
  auto &PP = CI->getPreprocessorOpts();
  PP.AllowPCHWithCompilerErrors = true;
  PP.DisablePCHOrModuleValidation = DisableValidationForModuleKind::PCH;
  PP.UsePredefines = false;
  PP.ImplicitPCHInclude = ItemSnapshot->Filename + ".pch";

  IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
      new llvm::vfs::OverlayFileSystem(VFS));
  Overlay->pushOverlay(ItemSnapshot->MemFS);
  VFS = Overlay;
  return true;
}

} // namespace clangd
} // namespace clang
