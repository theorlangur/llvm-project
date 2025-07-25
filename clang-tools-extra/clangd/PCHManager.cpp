#include "PCHManager.h"
#include "CompileCommands.h"
#include "Preamble.h"
#include "TUScheduler.h"
#include "support/Logger.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/OpenMPKinds.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/PrecompiledPreamble.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Serialization/ASTWriter.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <numeric>

namespace clang {
namespace clangd {
namespace {

class PrecompilePCHAction : public ASTFrontendAction {
public:
  PrecompilePCHAction(std::string *InMemStorage, PreambleCallbacks &Callbacks)
      : InMemStorage(InMemStorage), Callbacks(Callbacks) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef InFile) override;

  bool hasEmittedPreamblePCH() const { return HasEmittedPreamblePCH; }

  void setEmittedPreamblePCH(ASTWriter &Writer) {
    this->HasEmittedPreamblePCH = true;
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
  std::string *InMemStorage;
  PreambleCallbacks &Callbacks;
};

class PrecompilePCHConsumer : public PCHGenerator {
public:
  PrecompilePCHConsumer(PrecompilePCHAction &Action, Preprocessor &PP,
                        ModuleCache &ModuleCache, StringRef Isysroot,
                        const CodeGenOptions &CodeGenOpts,
                        std::unique_ptr<raw_ostream> Out)
      : PCHGenerator(PP, ModuleCache, "", Isysroot,
                     std::make_shared<PCHBuffer>(),
                     CodeGenOpts,
                     ArrayRef<std::shared_ptr<ModuleFileExtension>>(),
                     /*AllowASTWithErrors=*/true),
        Action(Action), Out(std::move(Out)) {}

  bool HandleTopLevelDecl(DeclGroupRef DG) override {
    Action.Callbacks.HandleTopLevelDecl(DG);
    return true;
  }

  void HandleTranslationUnit(ASTContext &Ctx) override {
    PCHGenerator::HandleTranslationUnit(Ctx);
    if (!hasEmittedPCH())
      return;

    // Write the generated bitstream to "Out".
    *Out << getPCH();
    // Make sure it hits disk now.
    Out->flush();
    // Free the buffer.
    llvm::SmallVector<char, 0> Empty;
    getPCH() = std::move(Empty);

    Action.setEmittedPreamblePCH(getWriter());
  }

  bool shouldSkipFunctionBody(Decl *D) override {
    return Action.Callbacks.shouldSkipFunctionBody(D);
  }

private:
  PrecompilePCHAction &Action;
  std::unique_ptr<raw_ostream> Out;
};

std::unique_ptr<ASTConsumer>
PrecompilePCHAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  std::string Sysroot;
  if (!GeneratePCHAction::ComputeASTConsumerArguments(CI, Sysroot))
    return nullptr;

  std::unique_ptr<llvm::raw_ostream> OS;
  if (InMemStorage) {
    OS = std::make_unique<llvm::raw_string_ostream>(*InMemStorage);
  } else {
    std::string OutputFile;
    OS = GeneratePCHAction::CreateOutputFile(CI, InFile, OutputFile);
  }
  if (!OS)
    return nullptr;

  if (!CI.getFrontendOpts().RelocatablePCH)
    Sysroot.clear();

  return std::make_unique<PrecompilePCHConsumer>(
      *this, CI.getPreprocessor(), CI.getModuleCache(), Sysroot, CI.getCodeGenOpts(), std::move(OS));
}

class CppFilePreambleCallbacks : public PreambleCallbacks {
public:
  CppFilePreambleCallbacks(PathRef File, PreambleParsedCallback ParsedCallback)
      : File(File), ParsedCallback(ParsedCallback) {}

  IncludeStructure takeIncludes() { return std::move(Includes); }

  CanonicalIncludes takeCanonicalIncludes() { return std::move(CanonIncludes); }

  include_cleaner::PragmaIncludes takePragmaIncludes() {
    return std::move(Pragmas);
  }

  void AfterExecute(CompilerInstance &CI) override {
    if (!ParsedCallback)
      return;
    trace::Span Tracer("Running PreambleCallback");
    ParsedCallback(CapturedASTCtx(CI), std::make_shared<const include_cleaner::PragmaIncludes>(takePragmaIncludes()));
  }

  void BeforeExecute(CompilerInstance &CI) override {
    CanonIncludes.addSystemHeadersMapping(CI.getLangOpts());
    LangOpts = &CI.getLangOpts();
    SourceMgr = &CI.getSourceManager();
    Pragmas.record(CI);
  }

  void InitiateIncludeCollection(CompilerInstance &CI)
  {
    Includes.collect(CI);
  }

  /*std::unique_ptr<PPCallbacks> createPPCallbacks() override {
    assert(SourceMgr && LangOpts &&
           "SourceMgr and LangOpts must be set at this point");

    return collectIncludeStructureCallback(*SourceMgr, &Includes);
  }*/

private:
  PathRef File;
  PreambleParsedCallback ParsedCallback;
  IncludeStructure Includes;
  CanonicalIncludes CanonIncludes;
  include_cleaner::PragmaIncludes Pragmas;
  const clang::LangOptions *LangOpts = nullptr;
  const SourceManager *SourceMgr = nullptr;
};
} // namespace

bool PCHManager::DbgLog = false;
PCHManager::PCHManager(const GlobalCompilationDatabase &CDB,
                       const ThreadsafeFS &TFS, ParsingCallbacks &Callbacks,
                       const Options &Opts
                       )
    : CDB(CDB), TFS(TFS), Callbacks(Callbacks),
      OnProgress(std::move(Opts.OnProgress)),
      CommandsChanged(
          CDB.watch([&](const std::vector<std::string> &ChangedFiles) {
            enqueue(ChangedFiles, FSType());
          })),
      PCHAnnounce(CDB.watch(
          [&](const std::vector<tooling::CompileCommand> &PCHAnnounced) {
            enqueue(PCHAnnounced);
          })),
      WaitForInit(Opts.WaitForInit),
      WorkspaceRoot(Opts.WorkspaceRoot)
{
  DbgLog = Opts.DbgLog;
  ThreadPool.runAsync("pch-worker",
                      [this, Ctx(Context::current().clone())]() mutable {
                        WithContext BGContext(std::move(Ctx));
                        Queue.work([&]{Queue.push(checkChangedPeriodically());});
                      });
}

PCHManager::~PCHManager() { Queue.stop(); }

PCHQueue::Task
PCHManager::changedFilesTask(const std::vector<std::string> &ChangedFiles,
                             FSType FS) {
  PCHQueue::Task T([this, ChangedFiles, FS] {
    unsigned Invalidated = invalidateAffectedPCH(ChangedFiles, FS);
    rebuildInvalidatedPCH(Invalidated, FS);
    updateAllHeaders();
  });

  T.ThreadPri = llvm::ThreadPriority::Default;
  return T;
}

PCHQueue::Task PCHManager::checkChangedPeriodically()
{
  PCHQueue::Task T([this] {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(500ms);
    if (!Changed.empty())
    {
      std::unique_lock<std::mutex> Lock(ChangedMtx);
      auto Dms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_t::now() - ChangedLastTime).count();
      if (Dms > 2000)
      {
        if (DbgLog)
          log("(PCH) sending changed file for processing ({0} items)", Changed.size());
        enqueue(Changed, ChangedFS);
        Changed.clear();
        ChangedFS = nullptr;
        return;
      }
    }

    Queue.push(checkChangedPeriodically());
  });
  return T;
}

std::optional<std::string> PCHManager::GetPCHCacheDirFor(PathRef File) const
{
  if (auto PI = CDB.getProjectInfo(File)) {
    llvm::SmallString<128> StorageDir;
    StorageDir = PI->SourceRoot;
    auto cacheDir = PI->ClangdCacheDir;
    if (cacheDir.empty()) cacheDir = "clangd";
    llvm::sys::path::append(StorageDir, ".cache", cacheDir.c_str(), "pch");
    return std::string(StorageDir.c_str());
  }else if (WorkspaceRoot)
  {
    llvm::SmallString<128> StorageDir;
    StorageDir = *WorkspaceRoot;
    auto cacheDir = PI->ClangdCacheDir;
    if (cacheDir.empty()) cacheDir = "clangd";
    llvm::sys::path::append(StorageDir, ".cache", cacheDir.c_str(), "pch");
    return std::string(StorageDir.c_str());
  }
  return {};
}

void PCHManager::checkChangedFile(PathRef File, FSType FS, bool force) {
  if (!Initialized)
  {
      if (DbgLog)
        log("(PCH) check changed file: not initialized yet");
      return;
  }

  {
    shared_lck SharedAccessToAllHeaders(UsedHeadersLock);
    if (!AllUsedHeaders.contains(File))
    {
      std::string LowerCase(File);
      if (LowerCase[0] != std::tolower(LowerCase[0]))
        LowerCase[0] = std::tolower(LowerCase[0]);
      else
        LowerCase[0] = std::toupper(LowerCase[0]);
      if (!AllUsedHeaders.contains(LowerCase)) {
        if (DbgLog)
          log("(PCH) file {0}/{1} doesn't affect any PCHs", File, LowerCase);
        return;
      }
    }
  }

  {
    std::unique_lock<std::mutex> Lock(ChangedMtx);
    ChangedLastTime = clock_t::now();
    if (!force)
      Changed.emplace_back(File);
    else
    {
      std::string f = "*";
      f += File;
      Changed.emplace_back(f);
    }
    ChangedFS = FS;
  }
}

void PCHManager::updateAllHeaders() {
  llvm::StringSet<> NewAllHeaders;
  {
    shared_lck SharedAccessToPCH(PCHLock);
    for (auto const &I : PCHs) {
      auto const &Headers = I->Includes;
      NewAllHeaders.insert(Headers.begin(), Headers.end());
      NewAllHeaders.insert(I->CompileCommand.Filename);
    }
  }

  std::unique_lock<std::shared_timed_mutex> ExclusiveAccessToAllHeaders(
      UsedHeadersLock);
  AllUsedHeaders = std::move(NewAllHeaders);
}

PCHQueue::Task PCHManager::announcedPCHTask(
    const std::vector<tooling::CompileCommand> &PCHAnnounced) {
  PCHQueue::Task T([this, PCHAnnounced] {
    if (!Initialized)
        log("(PCH) initializing ({0} announced)", PCHAnnounced.size());
    analyzePCHDependencies(std::move(PCHAnnounced));

    //at this point various findPCH calls are allowed
    //to find their PCHs and wait for them to be built
    if (!Initialized) {
      Initialized = true;
      InitCV.notify_all();
    }

    rebuildInvalidatedPCH((unsigned)PCHs.size(), FSType());
    log("(PCH) updated after announce");
    updateAllHeaders();
  });

  T.ThreadPri = llvm::ThreadPriority::Default;
  return T;
}

llvm::StringRef findPCHDependency(const tooling::CompileCommand &CC,
                                  size_t StartFromArg = 0, size_t N = std::string::npos) {
  auto beg = CC.CommandLine.begin() + StartFromArg;
  auto _end = N != std::string::npos ? beg + N : CC.CommandLine.end();
  for (auto i = beg; i != _end; ++i) {
    const auto &Arg = *i;
    if (Arg.find("__CLANGD_NO_PCH_DEP_NEXT__") != std::string::npos)
    {
      ++i;
      if (i == _end)
        break;

      continue;
    }
    size_t P = 0;
    while ((P = Arg.find("-include", P)) != std::string::npos) {
      if (!P || Arg[P - 1] == '-' ||
          Arg[P - 1] == ' ') // might be -include or --include
      {
        P += sizeof("-include") - 1;
        if (Arg[P] == '=')
          ++P; // skip =
        size_t FileStart;
        char endSym = ' ';
        if (Arg[P] == '"' || Arg[P] == '\'')
        {
          endSym = Arg[P];
          ++P;
        }
        FileStart = P;
        while ((P = Arg.find_first_of(endSym, P)) != std::string::npos) {
          if (Arg[P - 1] != '\\')
            break;
          ++P;
        }
        if (P == std::string::npos)
          P = Arg.size();
        return llvm::StringRef(Arg.data() + FileStart, P - FileStart);
      }
      ++P;
    }
  }
  return {};
}

bool hasArgStr(const tooling::CompileCommand &CC, const char *pSubStr) {
  for (const auto &a : CC.CommandLine) {
    if (a.find(pSubStr) != std::string::npos)
      return true;
  }
  return false;
}

std::optional<StringRef> getArgVal(const tooling::CompileCommand &CC,
                                   StringRef arg) {
  for (const auto &a : CC.CommandLine) {
    if (size_t i = a.find(arg); i != std::string::npos) {
      i += arg.size();
      if (a[i] == '=')
        ++i;
      return {StringRef(&a[i], a.size() - i)};
    }
  }
  return {};
}

llvm::StringRef findDynamicPCH(const tooling::CompileCommand &CC) {
  auto beg = CC.CommandLine.begin();
  auto _end = CC.CommandLine.end();
  for (auto i = beg; i != _end; ++i) {
    const auto &Arg = *i;
    if (Arg.find("__CLANGD_DYNAMIC_PCH__") != std::string::npos &&
        (i + 1 != _end)) {
      // next is expected to be the --include option
      return findPCHDependency(CC, std::distance(beg, i) + 1, 1);
    }
  }
  return {};
}

void PCHManager::analyzePCHDependencies(
    std::vector<tooling::CompileCommand> PCHCommands) {
  {
    // clear dynamics is easy and safe due to shared_ptr
    uniq_lck DynLoc(DynamicPCHLock);
    DynamicPCHs.clear();
  }

  {
    shared_lck ReadAccessToPCH(PCHLock);
    // need to invalidate everything
    for (auto &I : PCHs)
      I->invalidate(); // here we DO need to wait till usage is over
  }

  uniq_lck ExclusiveAccessToPCH(PCHLock);
  // removing
  PCHs.clear();
  PCHs.reserve(PCHCommands.size());
  //using namespace std::chrono_literals;
  //std::this_thread::sleep_for(20s);

  // 1. partiion PCH's with no dependencies
  auto NoDepIt = std::partition(PCHCommands.begin(), PCHCommands.end(),
                                [](const tooling::CompileCommand &CC) {
                                  return findPCHDependency(CC).empty();
                                });
  for (auto J = PCHCommands.begin(); J != NoDepIt; ++J)
    PCHs.emplace_back(std::make_shared<PCHItem>(*J));

  // 2. partition the rest till nothing is left
  auto Beg = PCHCommands.begin();
  auto End = PCHCommands.end();
  auto PrevPartBeg = Beg;
  auto PartBeg = NoDepIt;
  auto NewPartEnd = NoDepIt;
  while (NewPartEnd != End) {
    NewPartEnd =
        std::partition(PartBeg, End, [&](const tooling::CompileCommand &CC) {
          llvm::StringRef DepFName = findPCHDependency(CC);
          // partition to the left if this dependency is among previously
          // partitioned commands
          return std::find_if(PrevPartBeg, PartBeg,
                              [&](const tooling::CompileCommand &XX) {
                                return XX.Filename == DepFName;
                              }) != PartBeg;
        });

    for (auto J = PartBeg; J != NewPartEnd; ++J) {
      PCHs.emplace_back(std::make_shared<PCHItem>(*J));

      llvm::StringRef DepFName = findPCHDependency(*J);
      // translate range for pchCommands into pchs
      auto PCHsRangeBeg = PCHs.begin() + std::distance(Beg, PrevPartBeg);
      auto PCHsRangeEnd = PCHs.begin() + std::distance(Beg, PartBeg);
      // find the same PCHItem there
      auto Dep = std::find_if(PCHsRangeBeg, PCHsRangeEnd,
                              [&](const shared_pch_item &XX) {
                                return XX->CompileCommand.Filename == DepFName;
                              });
      (*Dep)->DependOnMe.push_back(PCHs.back());
      PCHs.back()->IdependOn.push_back(*Dep);
    }

    PrevPartBeg = PartBeg;
    PartBeg = NewPartEnd;
  }
}

unsigned PCHManager::PCHItem::invalidate() {
  unsigned Res = 0;
  { 
    uniq_lck fullLock(Lock);
    if (ItemState != PCHItem::State::Rebuild) {
      ++Res;
      ItemState = PCHItem::State::Rebuild;
      ++Version;
    }
  }

  if (Res) {
    // all dependencies must be invalidated
    for (auto& Dep : DependOnMe)
    {
      auto d = Dep.lock();//make shared out of weak
      if (d)
        Res += d->invalidate();
    }
  }
  return Res;
}

bool PCHManager::PCHItem::isAnyIncludeStateDifferent(FSType &VFS)
{
  for(auto& [path, v] : IncludeStates)
  {
    if (auto Status = VFS->status(path))
    {
      if (!v.compare(path, *Status))
      {
        if (DbgLog)
          elog("Include state different for {0}. Size: {1} vs {2}; Mod time: {3} vs {4}", path, v.Size, Status->getSize(), v.ModTime, Status->getLastModificationTime());
        return true;
      }
    }else if (llvm::sys::path::is_style_windows(llvm::sys::path::Style::native))
    {
      // check case
      std::string t = path.str();
      if (auto Status = VFS->status(t))
      {
        if (v == *Status)
          continue;
      }
      t[0] = std::toupper(path[0]);
      if (auto Status = VFS->status(t))
      {
        if (v == *Status)
          continue;
      }
      if (t[0] == path[0])
        t[0] = std::tolower(path[0]);

      if (auto Status = VFS->status(t))
      {
        if (!v.compare(path, *Status))
          return true;
      }
    }
  }
  return false;
}

bool PCHManager::PCHItem::IncFileState::compare(StringRef path, llvm::vfs::Status const& Status)
{
  if (*this != Status)
  {
    if (Size != Status.getSize())
      return true;

    if (md5 != llvm::MD5::MD5Result{})
    {
      if (auto tmd5 = llvm::sys::fs::md5_contents(path))
      {
        if(*tmd5 != md5)
          return true;
        //update ModTime to save on checks later
        ModTime = llvm::sys::toTimePoint(llvm::sys::toTimeT(Status.getLastModificationTime()));
        if (DbgLog)
          log("MD5 same, updating ModTime for {0}", path);
      }
    }
  }
  return false;
}

bool PCHManager::PCHItem::isIncludeStateDifferent(StringRef path,
                                                  FSType &VFS) {
  if (auto I = IncludeStates.find(path); I != IncludeStates.end()) {
    if (auto Status = VFS->status(path))
    {
      return I->getValue().compare(path, *Status);
    }else if (llvm::sys::path::is_style_windows(llvm::sys::path::Style::native))
    {
      // check case
      std::string t = path.str();
      t[0] = std::toupper(path[0]);
      if (t[0] == path[0])
        t[0] = std::tolower(path[0]);
      if (auto Status = VFS->status(t))
        return I->getValue().compare(path, *Status);
    }
  }
  return false;
}

void PCHManager::PCHItem::updateIncludeStates(FSType &VFS) {
  IncludeStates.clear();
  for (const auto &I : Includes) {
    if (auto S = VFS->status(I))
    {
      auto r = IncludeStates.insert_or_assign(I, *S);
      if (auto md5 = llvm::sys::fs::md5_contents(I))
        r.first->second.md5 = *md5;
      else
      {
        r.first->second.md5 = {};
        if (DbgLog)
          elog("Failed to get MD5 for {0}: {1}", I, md5.getError().message());
      }
    }
  }
  for (auto const &H : DynamicIncludes) {
    auto const &I = H.getKey();
    if (auto S = VFS->status(I))
    {
      auto r = IncludeStates.insert_or_assign(I, *S);
      if (auto md5 = llvm::sys::fs::md5_contents(I))
        r.first->second.md5 = *md5;
      else
      {
        (void)md5.getError();
        r.first->second.md5 = {};
        //elog("Failed to get MD5 for {0}: {1}", I, md5);
      }
    }
  }
}

PCHManager::PCHItem::IncFileState::IncFileState(llvm::vfs::Status const &s)
    : Size(s.getSize()), ModTime(llvm::sys::toTimePoint(llvm::sys::toTimeT(s.getLastModificationTime()))) {}

unsigned PCHManager::invalidateAffectedPCH(
    const std::vector<std::string> &ChangedFiles, FSType FSl) {
  unsigned Invalidated = 0;
  llvm::StringSet<> ChangedU;
  llvm::StringSet<> ChangedL;
  llvm::StringSet<> ForcedU;
  llvm::StringSet<> ForcedL;
  bool checkCase = llvm::sys::path::is_style_windows(llvm::sys::path::Style::native);
  for (auto S : ChangedFiles) {
    bool forced = S[0] == '*';
    if (forced)
      S.erase(0, 1);
    if (checkCase) {
      S[0] = std::toupper(S[0]);
      ChangedU.insert(S);
      if (forced)
        ForcedU.insert(S);
      S[0] = std::tolower(S[0]);
      ChangedL.insert(S);
      if (forced)
        ForcedL.insert(S);
    } else
    {
      ChangedU.insert(S);
      if (forced)
        ForcedU.insert(S);
    }
  }
  auto CheckChanged = [&](auto const &S)->bool {
    if (ChangedU.contains(S))
      return true;
    return checkCase && ChangedL.contains(S);
  };
  auto CheckForced = [&](auto const &S)->bool {
    if (ForcedU.contains(S))
      return true;
    return checkCase && ForcedL.contains(S);
  };
  {
    shared_lck ExclusiveAccessToPCH(PCHLock);
    for (auto &UI : PCHs) {
      PCHItem &Item = *UI;
      if (Item.ItemState == PCHItem::State::Rebuild)
        continue;

      if (CheckChanged(Item.CompileCommand.Filename)) {
        if (DbgLog)
          log("(PCH) invalidating {0} and all dependendents",
              Item.CompileCommand.Filename);
        Invalidated += Item.invalidate();
        continue;
      }

      for (const auto &S : Item.Includes) {
        if (CheckChanged(S)){
          if (DbgLog)
            log("(PCH) invalidating {0} and all dependendents because"
                " of the included (possible indirectly) {1} has changed",
                Item.CompileCommand.Filename, S);
          bool forced = CheckForced(S);
          if (forced || Item.isIncludeStateDifferent(S, FSl))
          {
              if (DbgLog)
                  log("(PCH) state of {0} in PCH is ignored since force is requested. Invalidating ", S);
              Invalidated += Item.invalidate();
              break;
          } else {
              if (DbgLog)
                log("(PCH) state of {0} in PCH is the same as the "
                    "current state in FS, so not invalidating anything...", S);
          }
        }
      }
    }
  }

  {
    shared_lck DynLock(DynamicPCHLock);
    for (auto &I : DynamicPCHs) {
      PCHItem &Item = *I.second;
      if (Item.ItemState == PCHItem::State::Rebuild)
        continue;

      if (!Item.IdependOn.empty() && Item.IdependOn[0]->ItemState == PCHItem::State::Rebuild) {
        Item.ItemState = PCHItem::State::Rebuild;
        ++Item.Version;
      } else {
        for (auto const &h : Item.DynamicIncludes) {
          const auto &I = h.getKey();
          if (CheckChanged(I)){
            if (DbgLog)
              log("(DynPCH) invalidating {0} and all dependendents "
                  "because"
                  " of the included (possible indirectly) {1} has "
                  "changed",
                  Item.CompileCommand.Filename, I);
            bool forced = CheckForced(I);
            if (forced || Item.isIncludeStateDifferent(I, FSl)) {
              if (DbgLog)
                log("(DynPCH) state of {0} in PCH is ignored "
                    "since "
                    "force is requested. Invalidating",
                    I);
              Invalidated += Item.invalidate(); 
                          // don't have to wait due to shared_ptr
                          // model of PCHData
              break;
            } else {
              if (DbgLog)
                log("(DynPCH) state of {0} in PCH is the same as "
                    "the "
                    "current state in FS, so not invalidating "
                    "anything...",
                    I);
            }
          }
        }
      }
    }
  }
  return Invalidated;
}
std::optional<PCHManager::PCHItem::IncFileState> PCHManager::PCHItem::IncFileState::read(StringRef &data)
{
  auto read64 = [&]{
    auto u64 = data.take_front(8);
    data = data.drop_front(8);
    return llvm::support::endian::read64le(u64.data());
  };
  IncFileState res;
  res.Size = read64();
  uint64_t unix = read64();
  res.ModTime = llvm::sys::toTimePoint(unix);
  auto md5data = data.take_front(sizeof(md5));
  data = data.drop_front(sizeof(md5));
  memcpy(&*res.md5.begin(), md5data.data(), sizeof(md5));
  return res;
}

void PCHManager::PCHItem::IncFileState::write(llvm::raw_ostream &os) const
{
  uint64_t sz = Size;
  os.write((const char*)&sz, sizeof(sz));
  uint64_t unixEpoch = llvm::sys::toTimeT(ModTime);
  os.write((const char*)&unixEpoch, sizeof(unixEpoch));
  os.write((const char*)md5.data(), sizeof(md5));
}

bool PCHManager::tryAddDynamicPCH(tooling::CompileCommand const &Cmd, FSType FS) {
  StringRef DynPCH = findDynamicPCH(Cmd);
  if (DynPCH.empty())
    return false;

  {
    shared_lck Lock(DynamicPCHLock);
    if (DynamicPCHs.find(Cmd.Filename) != DynamicPCHs.end()) // already in there
      return true;
  }

  StringRef PCHDep;
  auto CC = CDB.getCompileCommand(DynPCH);
  if (CC.has_value())
    PCHDep = findPCHDependency(*CC);

  PCHAccess depAccess = findPCH(PCHDep);
  // if (!depAccess)
  //   return false;

  {
    uniq_lck Lock(DynamicPCHLock);
    auto CC = Cmd;
    CC.Filename = DynPCH.str(); // actual compile command must be provided in
                                // compile commands database
    auto Item = std::make_shared<PCHItem>(CC);
    if (depAccess)
      Item->IdependOn.push_back(depAccess.ShItem);
    Item->Dynamic = true;
    DynamicPCHs[Cmd.Filename] = Item;
    if (DbgLog)
      log("(DynPCH) added dynamic PCH {0} for {1}", DynPCH, Cmd.Filename);
    Queue.push(PCHQueue::Task([this, FS] { rebuildInvalidatedPCH(1, FS); }));
  }

  return true;
}

bool PCHManager::tryRemoveDynamicPCH(tooling::CompileCommand const &Cmd) {
  StringRef PCHDep = findPCHDependency(Cmd);
  StringRef DynPCH = findDynamicPCH(Cmd);
  if ((DynPCH.empty() || PCHDep.empty()) || (DynPCH == PCHDep))
    return false;

  uniq_lck Lock(DynamicPCHLock); 

  if (DynamicPCHs.erase(Cmd.Filename) == 1) {
    if (DbgLog)
      log("(DynPCH) removed dynamic PCH {0} for {1}", DynPCH, Cmd.Filename);
    return true;
  }
  return false;
}

void PCHManager::addDynamicGhost(
    shared_pch_item Dep,
    IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> MemFS) {
    auto Buf = llvm::MemoryBuffer::getMemBuffer(
        "\nnamespace{};\n"); // bogus C++ content
    MemFS->addFile(Dep->CompileCommand.Filename, 0, std::move(Buf));
}

IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> 
PCHManager::collectDependencies(shared_pch_item Dep,
                                UsedPCHDataList &pchdatas) {
  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> PCHFS(
      new llvm::vfs::InMemoryFileSystem());

  if (Dep->Dynamic)
    addDynamicGhost(Dep, PCHFS);
  while (Dep) {
    auto data = std::atomic_load(&Dep->PCHData);
    if (data) {
		pchdatas.emplace_back(data);
		auto Buf = llvm::MemoryBuffer::getMemBuffer(*data);
		PCHFS->addFile(Dep->CompileCommand.Filename + ".pch", 0, std::move(Buf));
                Dep = Dep->IdependOn.empty() ? shared_pch_item()
                                             : Dep->IdependOn[0];
    }else {
      elog("(PCH) empty data on dependency {0}!!! (Status: {1})", Dep->CompileCommand.Filename, (int)Dep->ItemState);
    }
  }
  return PCHFS;
}

void PCHManager::makeSnapshot(shared_pch_item Item, PCHSnapshotPtr snap)
{
  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> PCHFS(
      new llvm::vfs::InMemoryFileSystem());
  snap->MemFS = PCHFS;
  if (Item->Dynamic)
    addDynamicGhost(Item, PCHFS);
  {
    auto Buf = llvm::MemoryBuffer::getMemBuffer(*snap->PCHData);
    PCHFS->addFile(snap->Filename + ".pch", 0, std::move(Buf));
  }

  size_t n = std::min(snap->PCHItems.size(), snap->UsedPCHDatasSnapshot.size());
  if (n != snap->PCHItems.size())
  {
    elog("(PCH) inconsistency between items and PCHdata. Items count={0}; Datas count={1}",
         snap->PCHItems.size(), snap->UsedPCHDatasSnapshot.size());
  }
  for (size_t i = 0; i < n; ++i)
  {
    auto &d = snap->PCHItems[i];
    auto &b = snap->UsedPCHDatasSnapshot[i];
    auto Buf = llvm::MemoryBuffer::getMemBuffer(*b);
    PCHFS->addFile(d->CompileCommand.Filename + ".pch", 0, std::move(Buf));
  }
}

IntrusiveRefCntPtr<llvm::vfs::FileSystem>
PCHManager::addDependencies(shared_pch_item Dep,
                            IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS, 
                            UsedPCHDataList &pchdatas) {
  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> PCHFS = collectDependencies(Dep, pchdatas);
  IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
      new llvm::vfs::OverlayFileSystem(VFS));

  Overlay->pushOverlay(PCHFS);
  return Overlay;
}

PPSkipIncludes::PPSkipIncludes(SourceManager &sm, StringRef target, bool skipTarget)
  : m_Target(target), m_SkipTarget(skipTarget) {

if (auto e = sm.getFileManager().getFileRef(m_Target))
  m_TargetRef = *e;
}

bool PPSkipIncludes::InclusionAllowed(SourceLocation HashLoc, const Token &IncludeTok,
							StringRef FileName, bool IsAngled,
							CharSourceRange FilenameRange,
							OptionalFileEntryRef File, StringRef SearchPath,
							StringRef RelativePath, const Module *Imported,
							SrcMgr::CharacteristicKind FileType) {
if (m_Skipped)
  return false;

if (m_TargetRef.has_value() && File.has_value() && *m_TargetRef == *File) {
  m_Skipped = true;
  if (!m_SkipTarget)
	  m_AllowedIncludes.insert((*File).getFileEntry().tryGetRealPathName().str());
  return !m_SkipTarget;
}
if (File.has_value())
	  m_AllowedIncludes.insert((*File).getFileEntry().tryGetRealPathName().str());
  return true;
}

PPSkipIncludes* PPSkipIncludes::CheckSkipIncludesArg(const tooling::CompileCommand& CC, clang::CompilerInstance* pCI)
{
  PPSkipIncludes *pPPSkipIncludes = nullptr;
  if (auto v = getArgVal(CC, "__CLANGD_PCH_SKIP__"))
  {
	pCI->getPreprocessor().addPPCallbacks(
		std::unique_ptr<PPCallbacks>(
			pPPSkipIncludes = new PPSkipIncludes(pCI->getSourceManager(), *v, true)
		));
  }
  return pPPSkipIncludes;
}

void PCHManager::rebuildPCH(shared_pch_item ShItem, FSType FS) {
  auto &Item = *ShItem;
  auto S = PCHItem::State::Invalid;
  auto OnExit = llvm::make_scope_exit([&] {
    if (OnProgress)
      OnProgress(Stats{++Complete, Total});
    OnPCHBuilt.broadcast(
        PCHEvent{Item.CompileCommand.Filename, S == PCHItem::State::Valid});
    Item.ItemState = S;
    Item.CV.notify_all();
  });

  uniq_lck ThisItemFullLock(Item.Lock);
  shared_pch_item Dep;
  if (!Item.IdependOn.empty()) 
      Dep = Item.IdependOn[0];

  shared_lck DepReadLock = Dep ? shared_lck(Dep->Lock) : shared_lck{};

  if (Dep && Dep->ItemState == PCHItem::State::Rebuild) {
        elog("(PCH)Cannot rebuild PCH for {0} as it depends on {1} which is "
             "in rebuild state",
             Item.CompileCommand.Filename, Dep->CompileCommand.Filename);
        Dep->CV.wait(DepReadLock,
                     [&] { return Dep->ItemState != PCHItem::State::Rebuild; });
  }
  if (Dep && Dep->ItemState != PCHItem::State::Valid) {
    Item.ItemState = PCHItem::State::Invalid;
        elog("(PCH)Cannot rebuild PCH for {0} as it depends on {1} which is "
             "invalid",
             Item.CompileCommand.Filename, Dep->CompileCommand.Filename);
        return;
  }

  PCHSnapshotPtr depSnapshot;
  PCHSnapshotPtr newSnapshot = std::make_shared<PCHSnapshot>();
  newSnapshot->Filename = Item.CompileCommand.Filename;
  if (Dep)
  {
      auto snap = Dep->PCHDatasSnapshot;
      depSnapshot = snap;
      
      newSnapshot->PCHItems = snap->PCHItems;
      newSnapshot->PCHItems.push_back(Dep);
      newSnapshot->UsedPCHDatasSnapshot = snap->UsedPCHDatasSnapshot;
      newSnapshot->UsedPCHDatasSnapshot.push_back(Dep->PCHData);
  }

  llvm::SmallString<128> pch_cache_path;
  if (auto pchCacheDir = GetPCHCacheDirFor(newSnapshot->Filename))
  {
    llvm::StringRef pchdir = *pchCacheDir;
    llvm::StringRef item_fn = newSnapshot->Filename;
    while(!item_fn.starts_with(pchdir)&& !pchdir.empty())
    {
      pchdir = llvm::sys::path::parent_path(pchdir);
    }
     llvm::SmallString<128> pch_name(item_fn.drop_front(pchdir.size()));
     pch_name += ".pch_cache";
     std::replace(pch_name.begin(), pch_name.end(), '/', '_');
     std::replace(pch_name.begin(), pch_name.end(), '\\', '_');
     std::replace(pch_name.begin(), pch_name.end(), ':', '_');
  
     pch_cache_path = *pchCacheDir;
     llvm::sys::path::append(pch_cache_path, pch_name);

    log("(PCH)For {0} considering cache at {1}",
        Item.CompileCommand.Filename, pch_cache_path);
  }else
  {
    log("(PCH)For {0} no cache is considered (no path)", Item.CompileCommand.Filename);
  }

  auto origV = Item.Version;
  if (!Item.PCHDatasSnapshot && !pch_cache_path.empty())
  {
    //no PCH data yet. Try loading one from cache
    if (llvm::sys::fs::exists(pch_cache_path))
    {
      auto Buffer = llvm::MemoryBuffer::getFile(pch_cache_path);
      if (Buffer)
      {
        auto b = Buffer->get()->getBuffer();
        PCHItem::DependencyVersions depsV;
        if (PCHItem::read(b, Item, depsV))
        {
          //verify if still valid based on stats
          auto vfs = TFS.view(Item.CompileCommand.Directory);
          bool diff = depsV.size() != Item.IdependOn.size();
          if (!diff && Item.isAnyIncludeStateDifferent(vfs))
            diff = true;
          if (!diff)
          {
            for(int i = 0, n = (int)depsV.size(); i < n; ++i)
            {
              auto d = Item.IdependOn[i];
              if (d->CompileCommand.Filename != depsV[i].fname || uint32_t(d->Version) != depsV[i].v)
              {
                diff = true;
                break;
              }
            }

            if (!diff)
            {
              //actually can be used
              newSnapshot->PCHData = Item.PCHData;
              newSnapshot->Version = Item.Version;
              makeSnapshot(ShItem, newSnapshot);
              std::atomic_store(&Item.PCHDatasSnapshot, newSnapshot);
              S = PCHItem::State::Valid;
              if (DbgLog)
                log("(PCH)Successfully loaded from cache precompiled header of size: {0} (file: {1}; Version: {2})",
                    Item.PCHData->size(), Item.CompileCommand.Filename, Item.Version);
              return;
            }
          }

          if (diff)
          {
              if (DbgLog)
                log("(PCH)Could not use cache file at {0} for {1}", pch_cache_path, Item.CompileCommand.Filename);
            ++Item.Version;
            if (Item.Version == origV)
              ++Item.Version;
          }
        }
      }
    }
  }

  ParseOptions Opts;

  ParseInputs Inputs;
  Inputs.TFS = &TFS;

  // auto FS = TFS.view(item.CompileCommand.Directory);
  // FS->getBufferForFile(getAbsolutePath(item.CompileCommand));
  // Inputs.Contents = TFS;

  auto CC = CDB.getCompileCommand(Item.CompileCommand.Filename);
  if (!CC) {
        elog("(PCH)Failed to get compile command for {0}",
             Item.CompileCommand.Filename);
        return;
  }

  Inputs.ForceRebuild = true;
  Inputs.Opts = std::move(Opts);
  Inputs.CompileCommand = *CC;

  std::string CCCmdLine;
  CCCmdLine = std::accumulate(
      CC->CommandLine.begin(), CC->CommandLine.end(), std::string(),
      [](std::string r, std::string arg) { return r + " " + arg; });
  if (DbgLog)
    log("(PCH) cmdline for {0}:\n{1}", Item.CompileCommand.Filename, CCCmdLine);

  StoreDiags CompilerInvocationDiagConsumer;
  std::vector<std::string> CC1Args;

  std::shared_ptr<CompilerInvocation> Invocation = std::shared_ptr<CompilerInvocation>(
      buildCompilerInvocation(Inputs, CompilerInvocationDiagConsumer, &CC1Args).release());
  if (!CC1Args.empty() && DbgLog)
        log("(PCH)Driver produced command: cc1 {0}", printArgv(CC1Args));

  auto &PreprocessorOpts = Invocation->getPreprocessorOpts();
  PreprocessorOpts.PrecompiledPreambleBytes.first = 0;
  PreprocessorOpts.PrecompiledPreambleBytes.second = false;
  PreprocessorOpts.DisablePCHOrModuleValidation =
      DisableValidationForModuleKind::PCH;
  PreprocessorOpts.WriteCommentListToPCH = false;
  // PreprocessorOpts.GeneratePreamble = true;

  auto VFS = TFS.view(Item.CompileCommand.Directory);
  if (FS) {
      if (DbgLog)
        log("(PCH) using passed FS as overlay");
      IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
          new llvm::vfs::OverlayFileSystem(VFS)); // passed VFS is primary

      if (Item.Dynamic) {
        IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> DynamicFS(
            new llvm::vfs::InMemoryFileSystem());
        addDynamicGhost(ShItem, DynamicFS);
        Overlay->pushOverlay(DynamicFS);
      }

      Overlay->pushOverlay(FS); // FS is the last one
      VFS = Overlay;
  }

  if (depSnapshot) {
        PreprocessorOpts.ImplicitPCHInclude =
            std::string(Dep->CompileCommand.Filename) + ".pch";
        IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
            new llvm::vfs::OverlayFileSystem(VFS));

        Overlay->pushOverlay(depSnapshot->MemFS);
        VFS = Overlay;
  }

  CppFilePreambleCallbacks SerializedDeclsCollector(
      Item.CompileCommand.Filename,
      [&](CapturedASTCtx AST, std::shared_ptr<const include_cleaner::PragmaIncludes> PragmaIncludes) {
        if (!Item.Dynamic) { // no symbol updates from dynamic pchs, those are
                             // limited by definition
          // call Callback.onPreambleAST
          Callbacks.onPreambleAST(Item.CompileCommand.Filename,
                                  std::to_string(Item.Version), std::move(AST), PragmaIncludes);
        }
      });
  PreambleCallbacks &Callbacks = SerializedDeclsCollector;
  llvm::SmallString<32> AbsFileName(Item.CompileCommand.Filename);
  VFS->makeAbsolute(AbsFileName);
  auto StatCache = std::make_unique<PreambleFileStatusCache>(AbsFileName);

  FrontendOptions &FrontendOpts = Invocation->getFrontendOpts();
  FrontendOpts.ProgramAction = frontend::GeneratePCH;
  FrontendOpts.SkipFunctionBodies = true;
  // FrontendOpts.OutputFile = "__in__memory___";

  std::vector<std::unique_ptr<FeatureModule::ASTListener>> ASTListeners;
  if (Inputs.FeatureModules) {
        for (auto &M : *Inputs.FeatureModules) {
      if (auto Listener = M.astListeners())
        ASTListeners.emplace_back(std::move(Listener));
        }
  }
  StoreDiags PreambleDiagnostics;
  PreambleDiagnostics.setDiagCallback(
      [&ASTListeners, &Item](const clang::Diagnostic &D, clangd::Diag &Diag) {
        if (Diag.Severity >= DiagnosticsEngine::Level::Error) {
          // scream here
          elog("Error while building PCH for {0}: {1}",
               Item.CompileCommand.Filename, Diag.Message);
        }
        llvm::for_each(ASTListeners,
                       [&](const auto &L) { L->sawDiagnostic(D, Diag); });
      });
  llvm::IntrusiveRefCntPtr<DiagnosticsEngine> PreambleDiagsEngine =
      CompilerInstance::createDiagnostics(*VFS, Invocation->getDiagnosticOpts(),
                                          &PreambleDiagnostics, false, nullptr);

  // Create the compiler instance to use for building the precompiled preamble.
  std::unique_ptr<CompilerInstance> Clang(new CompilerInstance(Invocation, std::make_shared<PCHContainerOperations>(), nullptr));

  // Recover resources if we crash before exiting this method.
  llvm::CrashRecoveryContextCleanupRegistrar<CompilerInstance> CICleanup(
      Clang.get());

  Clang->setDiagnostics(&*PreambleDiagsEngine);
  if (!Clang->createTarget()) {
        elog("(PCH)Failed to create clang traget for {0}",
             Item.CompileCommand.Filename);
        return; // BuildPreambleError::CouldntCreateTargetInfo;
  }

  if (Clang->getFrontendOpts().Inputs.size() != 1 ||
      Clang->getFrontendOpts().Inputs[ 0].getKind().getFormat() !=
          InputKind::Source ||
      Clang->getFrontendOpts().Inputs[0].getKind().getLanguage() ==
          Language::LLVM_IR) {
        elog("(PCH)Bad inputs for {0}", Item.CompileCommand.Filename);
        return; // BuildPreambleError::BadInputs;
  }

  // Create a file manager object to provide access to and cache the filesystem.
  Clang->setFileManager(new FileManager(Clang->getFileSystemOpts(), VFS));

  // Create the source manager.
  Clang->setSourceManager(
      new SourceManager(*PreambleDiagsEngine, Clang->getFileManager()));

  Clang->getLangOpts().CompilingPCH = true;
  Clang->createPreprocessor(TU_Prefix);

  std::shared_ptr<std::string> newPCH = std::make_shared<std::string>();
  std::unique_ptr<PrecompilePCHAction> Act;
  Act.reset(new PrecompilePCHAction(&*newPCH, Callbacks));
  Callbacks.BeforeExecute(*Clang);
  if (!Act->BeginSourceFile(*Clang.get(), Clang->getFrontendOpts().Inputs[0])) {
        elog("(PCH)Failed to start processing {0}",
             Item.CompileCommand.Filename);
        return; // BuildPreambleError::BeginSourceFileFailed;
  }
  SerializedDeclsCollector.InitiateIncludeCollection(*Clang);

  std::unique_ptr<PPCallbacks> DelegatedPPCallbacks =
      Callbacks.createPPCallbacks();
  if (DelegatedPPCallbacks)
        Clang->getPreprocessor().addPPCallbacks(
            std::move(DelegatedPPCallbacks));
  PPSkipIncludes *pPPSkipIncludes =
      PPSkipIncludes::CheckSkipIncludesArg(Inputs.CompileCommand, Clang.get());

  if (auto *CommentHandler = Callbacks.getCommentHandler())
        Clang->getPreprocessor().addCommentHandler(CommentHandler);

  if (llvm::Error Err = Act->Execute()) {
        elog("(PCH)Failure while executing clang for {0}: {1}",
             Item.CompileCommand.Filename,
             errorToErrorCode(std::move(Err)).message());
        return;
  }

  // Run the callbacks.
  Callbacks.AfterExecute(*Clang);

  Act->EndSourceFile();

  if (!Act->hasEmittedPreamblePCH()) {
        elog("(PCH)Could not emmit PCH for {0} (Version: {1})",
             Item.CompileCommand.Filename, Item.Version);
        return;
  }

  if (pPPSkipIncludes && !pPPSkipIncludes->WasSkipped())
        elog("(PCH)While generating {0} were expecting to skip {1} and further "
             "but didn't encounter",
             Item.CompileCommand.Filename, pPPSkipIncludes->GetTarget());

  Item.Includes = SerializedDeclsCollector.takeIncludes().takeAllHeaders();

  if (Item.Dynamic && pPPSkipIncludes)
      Item.DynamicIncludes = pPPSkipIncludes->takeAllowedIncludes();
  Item.updateIncludeStates(VFS);
  std::atomic_store(&Item.PCHData, newPCH);
  newSnapshot->PCHData = newPCH;
  newSnapshot->Version = Item.Version;
  makeSnapshot(ShItem, newSnapshot);
  std::atomic_store(&Item.PCHDatasSnapshot, newSnapshot);
  S = PCHItem::State::Valid;
  log("(PCH)Successfully generated precompiled header of size: {0} (file: {1}; Version: {2})",
      Item.PCHData->size(), Item.CompileCommand.Filename, Item.Version);

  // {
  //   log("(PCH)Dumping include states for {0}\r\n", Item.CompileCommand.Filename);
  //   for(auto const& [f, s] : Item.IncludeStates)
  //   {
  //     log("(PCH){0} : Size={1}; Mod={2}\r\n", f, s.Size, s.ModTime);
  //   }
  //   log("(PCH)Dump end for {0}\r\n", Item.CompileCommand.Filename);
  // }

  if (!pch_cache_path.empty())
  {
    llvm::sys::fs::create_directories(llvm::sys::path::parent_path(pch_cache_path));
    //TODO2: separately in background
    llvm::sys::fs::remove(pch_cache_path);
    auto err = llvm::writeToOutput(pch_cache_path, [&](llvm::raw_ostream &OS) {
        PCHItem::store_to(OS, Item);
        return llvm::Error::success();
    });
    if (err)
      elog("(PCH)Couldn't save PCH cache for {0} with error {1}", pch_cache_path, err);
    else
    {
      log("(PCH)Saved cache file at {0} for {1}", pch_cache_path, Item.CompileCommand.Filename);
      if (Item.isAnyIncludeStateDifferent(VFS))
      {
        elog("(PCH)Sanity check for PCH cache for {0} ({1}) failed. (diffs)", pch_cache_path, Item.CompileCommand.Filename);
      }
    }
  }
}

void PCHManager::PCHItem::store_to(llvm::raw_ostream &os, PCHItem const& i)
{
    auto writeStr = [&os](llvm::StringRef s)
    {
      uint32_t sz = s.size();
      os.write((const char*)&sz, sizeof(sz));
      os.write(s.data(), sz);
    };

    auto writeGen = [&os](auto v)
    {
      os.write((const char*)&v, sizeof(v));
    };


    uint32_t sz;
    writeGen(sz = i.Version);
    sz = i.Includes.size();
    writeGen(sz);
    for(auto const& v : i.Includes)
      writeStr(v);

    sz = i.DynamicIncludes.size();
    writeGen(sz);
    for(auto const& v : i.DynamicIncludes)
      writeStr(v.first());

    sz = i.IncludeStates.size();
    writeGen(sz);
    for(auto const& [k, v] : i.IncludeStates)
    {
      writeStr(k);
      v.write(os);
    }

    uint32_t iDepOnsz = i.IdependOn.size();
    writeGen(iDepOnsz);
    for(auto &d : i.IdependOn)
    {
      writeStr(d->CompileCommand.Filename);
      uint32_t v = d->Version;
      os.write((const char*)&v, sizeof(v));
    }

    writeStr(*i.PCHData);
    os.flush();
}

bool PCHManager::PCHItem::read(StringRef &s, PCHItem &i, DependencyVersions &deps)
{
  auto read32 = [&]{
    auto u32 = s.take_front(4);
    s = s.drop_front(4);
    return llvm::support::endian::read32le(u32.data());
  };
  //auto read64 = [&]{
  //  auto u64 = s.take_front(8);
  //  s = s.drop_front(8);
  //  return llvm::support::endian::read64le(u64.data());
  //};
  auto read_str_sz = [&](size_t sz){ 
    auto x = s.take_front(sz);
    s = s.drop_front(sz);
    return std::string(x.data(), sz); };
  auto read_str = [&]{ return read_str_sz(read32()); };

  uint32_t sz = read32();
  i.Version = sz;

  sz = read32();
  i.Includes.resize(sz);
  for(auto &s : i.Includes)
    s = read_str();

  i.DynamicIncludes.clear();
  for(size_t j = 0, n = read32(); j < n; ++j)
    i.DynamicIncludes.insert(read_str());

  i.IncludeStates.clear();
  for(size_t j = 0, n = read32(); j < n; ++j)
  {
    auto k = read_str();
    i.IncludeStates[k] = *IncFileState::read(s);
  }

  sz = read32();
  deps.resize(sz);
  for(uint32_t j = 0; j < sz; ++j)
  {
    auto n = read_str();
    auto v = read32();
    deps[j] = {std::move(n), v };
  }

  i.PCHData = std::make_shared<std::string>(read_str());
  return true;
}

void PCHManager::rebuildInvalidatedPCH(unsigned Total, FSType FS) {
  Complete = 0;
  this->Total = Total;
  if (OnProgress) OnProgress(Stats{Complete, Total});
  std::vector<std::thread> Builders;
  Builders.reserve(PCHs.size() + DynamicPCHs.size());
  for (auto &I : PCHs) {
    if (I->ItemState == PCHItem::State::Rebuild) {
      //rebuildPCH(*I, FS);
      Builders.emplace_back(
          [this,It=I,FS]() {
            rebuildPCH(It, FS);
          }
      );
    }
  }

  {
    shared_lck DynLock(DynamicPCHLock);

    for (auto &I : DynamicPCHs) {
      if (I.second->ItemState == PCHItem::State::Rebuild) {
        Builders.emplace_back(
            [this, It = I.second, FS]() { rebuildPCH(It, FS); });
      }
    }
  }

  for(auto &T : Builders)
    T.join();
}

PCHManager::PCHAccess
PCHManager::tryFindPCH(tooling::CompileCommand const &Cmd) const {
  if (!Initialized && !WaitForInit) {
    log("(tryFindPCH) is not initialized yet. Return empty for {0}", Cmd.Filename);
    return {};
  }
  return findPCH(Cmd);
}

PCHManager::PCHAccess PCHManager::tryFindDynPCH(tooling::CompileCommand const& Cmd) const {
  if (!Initialized && !WaitForInit) {
    log("(tryFindDynPCH) is not initialized yet. Return empty for {0}", Cmd.Filename);
    return {};
  }
  return findPCH(Cmd);
}

PCHManager::PCHAccess
PCHManager::tryFindPCH(clang::clangd::PathRef PCHFile) const {
  if (!Initialized && !WaitForInit) {
    log("(findPCH) is not initialized yet. Return empty for {0}", PCHFile);
    return {};
  }
  return findPCH(PCHFile);
}

bool PCHManager::hasPCHInDependencies(tooling::CompileCommand const& Cmd, PathRef PCHFile) const {
  StringRef DynPCH = findDynamicPCH(Cmd);
  if (!DynPCH.empty() && (DynPCH == PCHFile)) {
    shared_lck Lock(DynamicPCHLock);
    if (DbgLog)
      log("(PCH) hasPCHInDependencies request for {0} (PCH in question: {1}); "
          "Looking among dynamics",
          Cmd.Filename, PCHFile);
    for (const auto &I : DynamicPCHs) {
      if (I.second->CompileCommand.Filename == PCHFile) {
        if (DbgLog)
          log("(PCH) hasPCHInDependencies: found for {0} (PCH in question: {1}) "
              "among dynamics",
              Cmd.Filename, PCHFile);
        return true;
      }
    }
  }
  llvm::StringRef Dep = findPCHDependency(Cmd);
  if (!Dep.empty()) {
    shared_lck Lock(PCHLock);
    if (DbgLog)
      log("(PCH) hasPCHInDependencies request for {0} (PCH in question: {1})",
          Cmd.Filename, PCHFile);

    for (const auto &I : PCHs) {
      if (I->CompileCommand.Filename == Dep) {
        if (I->CompileCommand.Filename == PCHFile) // reacting only on main
        {
          if (DbgLog)
            log("(PCH) hasPCHInDependencies: found for {0} (PCH in question: "
                "{1})",
                Cmd.Filename, PCHFile);
          return true;
        }
        /*
        const PCHItem *pI = &*I;
        while (pI) {
          if (pI->CompileCommand.Filename == PCHFile) {
            return true;
          }
          pI = pI->IdependOn.empty() ? nullptr : pI->IdependOn[0];
        }
        */
      }
    }
  }
  return false;
}

PCHManager::PCHAccess
PCHManager::findPCH(tooling::CompileCommand const &Cmd) const {
  StringRef DynPCH = findDynamicPCH(Cmd);
  if (!DynPCH.empty()) {
    auto pchAccess = findDynPCH(DynPCH);
    if (pchAccess)
      return pchAccess;
  }

  llvm::StringRef Dep = findPCHDependency(Cmd);
  if (!Dep.empty())
    return findPCH(Dep);
  return {};
}

PCHManager::PCHAccess
PCHManager::findDynPCH(clang::clangd::PathRef PCHFile) const {
  if (!Initialized) {
    log("(findPCH) is not initialized yet. Waiting for initialization...");
    shared_lck Lock(PCHLock);
    InitCV.wait(Lock, [&] { return Initialized.load(); });
  }

  shared_pch_item res;
  {
    shared_lck Lock(DynamicPCHLock);
    if (DbgLog)
      vlog("(findDynamicPCH) find request for {0}", PCHFile);
    for (const auto &It : DynamicPCHs) {
      auto I = It.second;
      if (I->CompileCommand.Filename == PCHFile) {
        res = I;
        break;
      }
    }
  }

  if (res)
  {
    auto snap = res->PCHDatasSnapshot;
    auto I = res;
    if (!snap)
    {
      shared_lck ItemLock(res->Lock);
      if (I->ItemState == PCHItem::State::Rebuild) {
        I->CV.wait(ItemLock,
                   [&] { return I->ItemState != PCHItem::State::Rebuild; });
      }

      if (I->ItemState == PCHItem::State::Invalid)
        return {};
    }
    
    if (DbgLog)
      vlog("(findDynamicPCH) found request for {0}", PCHFile);
    return PCHAccess(snap, res, const_cast<PCHManager *>(this));
  }
  return {};
}

PCHManager::PCHAccess
PCHManager::findPCH(clang::clangd::PathRef PCHFile) const {
  // return {};
  if (!Initialized) {
    log("(findPCH) is not initialized yet. Waiting for initialization...");
    shared_lck Lock(PCHLock);
    InitCV.wait(Lock, [&] { return Initialized.load(); });
  }
  shared_pch_item res;

  {
    shared_lck Lock(PCHLock);
    if (DbgLog)
      vlog("(findPCH) find request for {0}", PCHFile);

    for (const auto &I : PCHs) {
      if (I->CompileCommand.Filename == PCHFile) {
        res = I;
        break;
      }
    }
  }

  if (res)
  {
    auto snap = res->PCHDatasSnapshot;
    auto I = res;
    if (!snap) {
      shared_lck ItemLock(res->Lock);
      if (I->ItemState == PCHItem::State::Rebuild) {
        I->CV.wait(ItemLock,
                   [&] { return I->ItemState != PCHItem::State::Rebuild; });
      }

      if (I->ItemState == PCHItem::State::Invalid)
        return {};
    }

    if (DbgLog)
      vlog("(findPCH) found request for {0}", PCHFile);
    return PCHAccess(snap, res, const_cast<PCHManager *>(this));
  }
  return {};
}

// PCHManager::PCHAccess
bool PCHManager::PCHAccess::addPCH(
    CompilerInvocation *CI,
    IntrusiveRefCntPtr<llvm::vfs::FileSystem> &VFS) const {
  if (itemSnapshot) {
    auto &pp = CI->getPreprocessorOpts();
    pp.AllowPCHWithCompilerErrors = true;
    pp.DisablePCHOrModuleValidation =
        DisableValidationForModuleKind::PCH;
    pp.UsePredefines = false;
    pp.ImplicitPCHInclude =
        std::string(itemSnapshot->Filename) + ".pch";

    IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
        new llvm::vfs::OverlayFileSystem(VFS));

    Overlay->pushOverlay(itemSnapshot->MemFS);
    VFS = Overlay;
    return true;
  }
  return false;
}


PCHManager::PCHAccess::PCHAccess(shared_pch_item ShItem, PCHManager *pMgr,
                                 shared_lck itemLock)
    : ShItem(ShItem), Item(ShItem.get()), pManager(pMgr), ItemReadLock(std::move(itemLock)) {
    ++Item->InUse;
    itemSnapshot = ShItem->PCHDatasSnapshot;
}

PCHManager::PCHAccess::PCHAccess(PCHSnapshotPtr itemSnapshot,
                                 shared_pch_item ShItem, PCHManager *pMgr)
    : ShItem(ShItem), pManager(pMgr), itemSnapshot(itemSnapshot) {

}

PCHManager::PCHAccess::PCHAccess(PCHAccess &&Rhs) : 
    ShItem(std::move(Rhs.ShItem)), 
    Item(Rhs.Item), 
    pManager(Rhs.pManager),
    ItemReadLock(std::move(Rhs.ItemReadLock)),
    itemSnapshot(std::move(Rhs.itemSnapshot))
{
  Rhs.Item = nullptr;
}
PCHManager::PCHAccess::~PCHAccess() {
  if (Item && (Item->InUse.fetch_sub(1) == 1))
    Item->CV.notify_all();
}

PCHManager::PCHAccess &PCHManager::PCHAccess::operator=(PCHAccess &&Rhs) {
  Item = Rhs.Item;
  Rhs.Item = nullptr;
  ShItem = std::move(Rhs.ShItem);
  ItemReadLock = std::move(Rhs.ItemReadLock);
  itemSnapshot = std::move(Rhs.itemSnapshot);
  return *this;
}

} // namespace clangd
} // namespace clang
