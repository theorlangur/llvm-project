#include "PCHManager.h"
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
#include "llvm/ADT/None.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

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
  PrecompilePCHConsumer(PrecompilePCHAction &Action, const Preprocessor &PP,
                        InMemoryModuleCache &ModuleCache, StringRef Isysroot,
                        std::unique_ptr<raw_ostream> Out)
      : PCHGenerator(PP, ModuleCache, "", Isysroot,
                     std::make_shared<PCHBuffer>(),
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
      *this, CI.getPreprocessor(), CI.getModuleCache(), Sysroot, std::move(OS));
}

class CppFilePreambleCallbacks : public PreambleCallbacks {
public:
  CppFilePreambleCallbacks(PathRef File, PreambleParsedCallback ParsedCallback)
      : File(File), ParsedCallback(ParsedCallback) {}

  IncludeStructure takeIncludes() { return std::move(Includes); }

  CanonicalIncludes takeCanonicalIncludes() { return std::move(CanonIncludes); }

  void AfterExecute(CompilerInstance &CI) override {
    if (!ParsedCallback)
      return;
    trace::Span Tracer("Running PreambleCallback");
    ParsedCallback(CI.getASTContext(), CI.getPreprocessorPtr(), CanonIncludes);
  }

  void BeforeExecute(CompilerInstance &CI) override {
    CanonIncludes.addSystemHeadersMapping(CI.getLangOpts());
    LangOpts = &CI.getLangOpts();
    SourceMgr = &CI.getSourceManager();
  }

  std::unique_ptr<PPCallbacks> createPPCallbacks() override {
    assert(SourceMgr && LangOpts &&
           "SourceMgr and LangOpts must be set at this point");

    return collectIncludeStructureCallback(*SourceMgr, &Includes);
  }

private:
  PathRef File;
  PreambleParsedCallback ParsedCallback;
  IncludeStructure Includes;
  CanonicalIncludes CanonIncludes;
  const clang::LangOptions *LangOpts = nullptr;
  const SourceManager *SourceMgr = nullptr;
};
} // namespace

PCHManager::PCHManager(const GlobalCompilationDatabase &CDB,
                       const ThreadsafeFS &TFS, ParsingCallbacks &Callbacks,
                       const Options &Opts)
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
      WaitForInit(Opts.WaitForInit) {
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
    unsigned Invalidated = invalidateAffectedPCH(ChangedFiles);
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

void PCHManager::checkChangedFile(PathRef File, FSType FS) {
  if (!Initialized)
  {
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
        log("(PCH) file {0}/{1} doesn't affect any PCHs", File, LowerCase);
        return;
      }
    }
  }

  {
    std::unique_lock<std::mutex> Lock(ChangedMtx);
    ChangedLastTime = clock_t::now();
    Changed.emplace_back(File);
    ChangedFS = FS;
  }
}

void PCHManager::updateAllHeaders() {
  llvm::StringSet<> NewAllHeaders;
  {
    shared_lck SharedAccessToPCH(PCHLock);
    for (auto const &I : PCHs) {
      auto const &Headers = I->Includes.allHeaders();
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
    size_t P = 0;
    while ((P = Arg.find("-include", P)) != std::string::npos) {
      if (!P || Arg[P - 1] == '-' ||
          Arg[P - 1] == ' ') // might be -include or --include
      {
        P += sizeof("-include") - 1;
        if (Arg[P] == '=')
          ++P; // skip =
        size_t FileStart = P;
        while ((P = Arg.find_first_of(' ', P)) != std::string::npos) {
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

  uniq_lck ExclusiveAccessToPCH(PCHLock);
  // need to invalidate everything
  for (auto &I : PCHs)
    I->invalidate(ExclusiveAccessToPCH, true);//here we DO need to wait till usage is over

  // removing
  PCHs.clear();
  PCHs.reserve(PCHCommands.size());
  // 1. partiion PCH's with no dependencies
  auto Beg = PCHCommands.begin();
  auto End = PCHCommands.end();
  auto NoDepIt = std::partition(PCHCommands.begin(), PCHCommands.end(),
                                [](const tooling::CompileCommand &CC) {
                                  return findPCHDependency(CC).empty();
                                });
  for (auto J = PCHCommands.begin(); J != NoDepIt; ++J)
    PCHs.emplace_back(std::make_unique<PCHItem>(*J));

  // 2. partition the rest till nothing is left
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
      PCHs.emplace_back(std::make_unique<PCHItem>(*J));

      llvm::StringRef DepFName = findPCHDependency(*J);
      // translate range for pchCommands into pchs
      auto PCHsRangeBeg = PCHs.begin() + std::distance(Beg, PrevPartBeg);
      auto PCHsRangeEnd = PCHs.begin() + std::distance(Beg, PartBeg);
      // find the same PCHItem there
      auto Dep = std::find_if(PCHsRangeBeg, PCHsRangeEnd,
                              [&](const std::unique_ptr<PCHItem> &XX) {
                                return XX->CompileCommand.Filename == DepFName;
                              });
      (*Dep)->DependOnMe.push_back(&*(PCHs.back()));
      PCHs.back()->IdependOn.push_back(&(**Dep));
    }

    PrevPartBeg = PartBeg;
    PartBeg = NewPartEnd;
  }
}

unsigned PCHManager::PCHItem::invalidate(uniq_lck &Lock, bool WaitForNoUsage) {
  unsigned Res = 0;
  if (ItemState != PCHItem::State::Rebuild) {
    ++Res;
    if (WaitForNoUsage && (ItemState == PCHItem::State::Valid) && (InUse > 0))
      CV.wait(Lock, [&] { return InUse == 0; });

    ItemState = PCHItem::State::Rebuild;
    ++Version;
    // all dependencies must be invalidated
    for (PCHItem *Dep : DependOnMe)
      Res += Dep->invalidate(Lock, WaitForNoUsage);
  }
  return Res;
}

unsigned PCHManager::invalidateAffectedPCH(
    const std::vector<std::string> &ChangedFiles) {
  unsigned Invalidated = 0;
  {
    uniq_lck ExclusiveAccessToPCH(PCHLock);
    llvm::StringSet<> ChangedU;
    llvm::StringSet<> ChangedL;
    for (auto S : ChangedFiles) {
      S[0] = std::toupper(S[0]);
      ChangedU.insert(S);
      S[0] = std::tolower(S[0]);
      ChangedL.insert(S);
    }
    for (auto &UI : PCHs) {
      PCHItem &Item = *UI;
      if (Item.ItemState == PCHItem::State::Rebuild)
        continue;

      if (ChangedU.contains(Item.CompileCommand.Filename) ||
          ChangedL.contains(Item.CompileCommand.Filename)) {
        log("(PCH) invalidating {0} and all dependendents",
            Item.CompileCommand.Filename);
        Invalidated += Item.invalidate(ExclusiveAccessToPCH, false);//don't have to wait due to shared_ptr model of PCHData
        continue;
      }

      for (const auto &S : Item.Includes.allHeaders()) {
        if (ChangedU.contains(S) || ChangedL.contains(S)) {
          log("(PCH) invalidating {0} and all dependendents because"
              " of the included (possible indirectly) {1} has changed",
              Item.CompileCommand.Filename, S);
          Invalidated += Item.invalidate(ExclusiveAccessToPCH, false);//don't have to wait due to shared_ptr model of PCHData
          break;
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

      if (Item.IdependOn[0]->ItemState == PCHItem::State::Rebuild) {
        Item.ItemState = PCHItem::State::Rebuild;
        ++Item.Version;
      }
    }
  }
  return Invalidated;
}

bool PCHManager::tryAddDynamicPCH(tooling::CompileCommand const &Cmd, FSType FS) {
  StringRef PCHDep = findPCHDependency(Cmd);
  StringRef DynPCH = findDynamicPCH(Cmd);
  if ((DynPCH.empty() || PCHDep.empty()) || (DynPCH == PCHDep))
    return false;

  {
    shared_lck Lock(DynamicPCHLock);
    if (DynamicPCHs.find(Cmd.Filename) != DynamicPCHs.end()) // already in there
      return true;
  }

  PCHAccess depAccess = findPCH(PCHDep);
  if (!depAccess)
    return false;

  {
    uniq_lck Lock(DynamicPCHLock);
    auto CC = Cmd;
    CC.Filename = DynPCH.str();//actual compile command must be provided in compile commands database
    auto Item = std::make_shared<PCHItem>(CC);
    Item->IdependOn.push_back(const_cast<PCHItem*>(depAccess.Item));
    Item->Dynamic = true;
    DynamicPCHs[Cmd.Filename] = Item;
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
  return DynamicPCHs.erase(Cmd.Filename) == 1;
}

IntrusiveRefCntPtr<llvm::vfs::FileSystem>
PCHManager::addDependencies(const PCHItem *Dep,
                            IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS, 
                            UsedPCHDataList &pchdatas) {
  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> PCHFS(
      new llvm::vfs::InMemoryFileSystem());
  while (Dep) {
    auto data = std::atomic_load(&Dep->PCHData);
    if (data) {
		pchdatas.emplace_back(data);
		auto Buf = llvm::MemoryBuffer::getMemBuffer(*data);
		PCHFS->addFile(Dep->CompileCommand.Filename + ".pch", 0, std::move(Buf));
		Dep = Dep->IdependOn.empty() ? nullptr : Dep->IdependOn[0];
    }else {
      elog("(PCH) empty data on dependency {0}!!! (Status: {1})", Dep->CompileCommand.Filename, (int)Dep->ItemState);
    }
  }

  IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
      new llvm::vfs::OverlayFileSystem(VFS));

  Overlay->pushOverlay(PCHFS);
  return Overlay;
}

void PCHManager::rebuildPCH(PCHItem &Item, FSType FS) {
  auto S = PCHItem::State::Invalid;
  auto OnExit = llvm::make_scope_exit([&] {
    if (OnProgress)
      OnProgress(Stats{++Complete, Total});
    Item.ItemState = S;
    Item.CV.notify_all();
    OnPCHBuilt.broadcast(
        PCHEvent{Item.CompileCommand.Filename, S == PCHItem::State::Valid});
  });

  PCHItem *Dep = Item.IdependOn.empty() ? nullptr : Item.IdependOn[0];
  if (Dep && Dep->ItemState == PCHItem::State::Rebuild) {
    elog("(PCH)Cannot rebuild PCH for {0} as it depends on {1} which is "
         "in rebuild state",
         Item.CompileCommand.Filename, Dep->CompileCommand.Filename);
    
    shared_lck Lock(PCHLock);
    Dep->CV.wait(Lock, [&] { return Dep->ItemState != PCHItem::State::Rebuild; });
  }
  if (Dep && Dep->ItemState != PCHItem::State::Valid) {
    elog("(PCH)Cannot rebuild PCH for {0} as it depends on {1} which is "
         "invalid",
         Item.CompileCommand.Filename, Dep->CompileCommand.Filename);
    return;
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

  StoreDiags CompilerInvocationDiagConsumer;
  std::vector<std::string> CC1Args;

  std::unique_ptr<CompilerInvocation> Invocation =
      buildCompilerInvocation(Inputs, CompilerInvocationDiagConsumer, &CC1Args);
  if (!CC1Args.empty())
    log("(PCH)Driver produced command: cc1 {0}", printArgv(CC1Args));

  auto &PreprocessorOpts = Invocation->getPreprocessorOpts();
  PreprocessorOpts.PrecompiledPreambleBytes.first = 0;
  PreprocessorOpts.PrecompiledPreambleBytes.second = false;
  PreprocessorOpts.DisablePCHOrModuleValidation =
      DisableValidationForModuleKind::PCH;
  PreprocessorOpts.WriteCommentListToPCH = false;
  //PreprocessorOpts.GeneratePreamble = true;

  auto VFS = TFS.view(Item.CompileCommand.Directory);
  if (FS) {
    log("(PCH) using passed FS as overlay");
    IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
        new llvm::vfs::OverlayFileSystem(VFS)); // passed VFS is primary
    Overlay->pushOverlay(FS);                 // FS is secondary
    VFS = Overlay;
  }

  UsedPCHDataList UsedPCHDatas;
  if (Dep) {
    PreprocessorOpts.ImplicitPCHInclude =
        std::string(Dep->CompileCommand.Filename) + ".pch";
    VFS = addDependencies(Dep, VFS, UsedPCHDatas);
  }

  CppFilePreambleCallbacks SerializedDeclsCollector(
      Item.CompileCommand.Filename,
      [&](ASTContext &AST, std::shared_ptr<clang::Preprocessor> PP,
          const CanonicalIncludes &CanInc) {
        // call Callback.onPreambleAST
        Callbacks.onPreambleAST(Item.CompileCommand.Filename, std::to_string(Item.Version), AST,
                                std::move(PP), CanInc);
      });
  PreambleCallbacks &Callbacks = SerializedDeclsCollector;
  llvm::SmallString<32> AbsFileName(Item.CompileCommand.Filename);
  VFS->makeAbsolute(AbsFileName);
  auto StatCache = std::make_unique<PreambleFileStatusCache>(AbsFileName);

  FrontendOptions &FrontendOpts = Invocation->getFrontendOpts();
  FrontendOpts.ProgramAction = frontend::GeneratePCH;
  FrontendOpts.SkipFunctionBodies = true;
  //FrontendOpts.OutputFile = "__in__memory___";

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
      CompilerInstance::createDiagnostics(&Invocation->getDiagnosticOpts(),
                                          &PreambleDiagnostics, false);

  std::shared_ptr<PCHContainerOperations> PCHContainerOps =
      std::make_shared<PCHContainerOperations>();

  // Create the compiler instance to use for building the precompiled preamble.
  std::unique_ptr<CompilerInstance> Clang(
      new CompilerInstance(std::move(PCHContainerOps)));

  // Recover resources if we crash before exiting this method.
  llvm::CrashRecoveryContextCleanupRegistrar<CompilerInstance> CICleanup(
      Clang.get());

  Clang->setInvocation(std::move(Invocation));
  Clang->setDiagnostics(&*PreambleDiagsEngine);
  if (!Clang->createTarget()) {
    elog("(PCH)Failed to create clang traget for {0}",
         Item.CompileCommand.Filename);
    return; // BuildPreambleError::CouldntCreateTargetInfo;
  }

  if (Clang->getFrontendOpts().Inputs.size() != 1 ||
      Clang->getFrontendOpts().Inputs[0].getKind().getFormat() !=
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

  std::shared_ptr<std::string> newPCH = std::make_shared<std::string>();
  std::unique_ptr<PrecompilePCHAction> Act;
  Act.reset(new PrecompilePCHAction(&*newPCH, Callbacks));
  Callbacks.BeforeExecute(*Clang);
  if (!Act->BeginSourceFile(*Clang.get(), Clang->getFrontendOpts().Inputs[0])) {
    elog("(PCH)Failed to start processing {0}", Item.CompileCommand.Filename);
    return; // BuildPreambleError::BeginSourceFileFailed;
  }

  std::unique_ptr<PPCallbacks> DelegatedPPCallbacks =
      Callbacks.createPPCallbacks();
  if (DelegatedPPCallbacks)
    Clang->getPreprocessor().addPPCallbacks(std::move(DelegatedPPCallbacks));
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
    elog("(PCH)Could not emmit PCH for {0} (Version: {1})", Item.CompileCommand.Filename, Item.Version);
    return;
  }

  Item.Includes = SerializedDeclsCollector.takeIncludes();
  Item.CanonIncludes = SerializedDeclsCollector.takeCanonicalIncludes();
  std::atomic_store(&Item.PCHData, newPCH);
  S = PCHItem::State::Valid;
  log("(PCH)Successfully generated precompiled header of size: {0} (file: {1}; Version: {2})",
      Item.PCHData->size(), Item.CompileCommand.Filename, Item.Version);
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
          [this,It=&*I,FS]() {
            rebuildPCH(*It, FS);
          }
      );
    }
  }

  {
    shared_lck DynLock(DynamicPCHLock);

    for (auto &I : DynamicPCHs) {
      if (I.second->ItemState == PCHItem::State::Rebuild) {
        Builders.emplace_back(
            [this, It = I.second, FS]() { rebuildPCH(*It.get(), FS); });
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
    log("(PCH) hasPCHInDependencies request for {0} (PCH in question: {1}); "
        "Looking among dynamics",
        Cmd.Filename, PCHFile);
    for (const auto &I : DynamicPCHs) {
      if (I.second->CompileCommand.Filename == PCHFile) {
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
    log("(PCH) hasPCHInDependencies request for {0} (PCH in question: {1})",
        Cmd.Filename, PCHFile);

    for (const auto &I : PCHs) {
      if (I->CompileCommand.Filename == Dep) {
        if (I->CompileCommand.Filename == PCHFile) // reacting only on main
        {
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
  // return {};
  /* if (llvm::StringRef(Cmd.Filename).endswith(".h"))
  {
    log("(findPCH) turned off for headers. {0}", Cmd.Filename);
    return {};
  }*/
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
  shared_lck Lock(DynamicPCHLock);
  vlog("(findDynamicPCH) find request for {0}", PCHFile);
  for (const auto &It : DynamicPCHs) {
    auto I = It.second;
    if (I->CompileCommand.Filename == PCHFile) {
      if (I->ItemState == PCHItem::State::Rebuild) {
        auto data = std::atomic_load(&I->PCHData);
        if (data->empty()) // if it's empty - we wait. If there's some old PCH -
                           // we use it right away to avoid delays
          I->CV.wait(Lock,
                     [&] { return I->ItemState != PCHItem::State::Rebuild; });
      }

      if (I->ItemState == PCHItem::State::Invalid)
        return {};

      vlog("(findDynamicPCH) found request for {0}", PCHFile);
      return PCHAccess(I);
    }
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
  shared_lck Lock(PCHLock);
  vlog("(findPCH) find request for {0}", PCHFile);

  for (const auto &I : PCHs) {
    if (I->CompileCommand.Filename == PCHFile) {
      if (I->ItemState == PCHItem::State::Rebuild) {
        auto data = std::atomic_load(&I->PCHData);
        if (data->empty())//if it's empty - we wait. If there's some old PCH - we use it right away to avoid delays
			I->CV.wait(Lock, [&] { return I->ItemState != PCHItem::State::Rebuild; });
      }

      if (I->ItemState == PCHItem::State::Invalid)
        return {};

      vlog("(findPCH) found request for {0}", PCHFile);
      return PCHAccess(I.get());
    }
  }
  return {};
}

// PCHManager::PCHAccess
bool PCHManager::PCHAccess::addPCH(
    CompilerInvocation *CI,
    IntrusiveRefCntPtr<llvm::vfs::FileSystem> &VFS) const {
  if (Item) {
    auto &pp = CI->getPreprocessorOpts();
    pp.AllowPCHWithCompilerErrors = true;
    pp.DisablePCHOrModuleValidation =
        DisableValidationForModuleKind::PCH;
    pp.ImplicitPCHInclude =
        std::string(Item->CompileCommand.Filename) + ".pch";
    VFS = addDependencies(Item, VFS, UsedPCHDatas);
    return true;
  }
  return false;
}

PCHManager::PCHAccess::PCHAccess(const PCHItem *Item) : Item(Item) {
  if (Item)
    ++Item->InUse;
}

PCHManager::PCHAccess::PCHAccess(std::shared_ptr<PCHItem> ShItem)
    : Item(ShItem.get()), DynItem(ShItem) {
  if (Item) {
    ++Item->InUse;
  }
}

PCHManager::PCHAccess::PCHAccess(PCHAccess &&Rhs) : DynItem(std::move(Rhs.DynItem)), Item(Rhs.Item), UsedPCHDatas(std::move(Rhs.UsedPCHDatas)) {
  Rhs.Item = nullptr;
}
PCHManager::PCHAccess::~PCHAccess() {
  if (Item && (Item->InUse.fetch_sub(1) == 1))
    Item->CV.notify_all();
}

PCHManager::PCHAccess &PCHManager::PCHAccess::operator=(PCHAccess &&Rhs) {
  std::swap(Item, Rhs.Item);
  UsedPCHDatas = std::move(Rhs.UsedPCHDatas);
  DynItem = std::move(Rhs.DynItem);
  return *this;
}

} // namespace clangd
} // namespace clang