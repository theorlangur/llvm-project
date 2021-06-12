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
                        Queue.work({});
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

void PCHManager::checkChangedFile(PathRef File, FSType FS) {
  if (!Initialized)
    return;

  {
    shared_lck SharedAccessToAllHeaders(UsedHeadersLock);
    if (!AllUsedHeaders.contains(File))
      return;
  }

  std::vector<std::string> Changed;
  Changed.emplace_back(File);
  enqueue(Changed, FS);
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
    log("(PCH) updatead after announce");
    updateAllHeaders();
  });

  T.ThreadPri = llvm::ThreadPriority::Default;
  return T;
}

llvm::StringRef findPCHDependency(const tooling::CompileCommand &CC) {
  for (const auto &Arg : CC.CommandLine) {
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

void PCHManager::analyzePCHDependencies(
    std::vector<tooling::CompileCommand> PCHCommands) {
  uniq_lck ExclusiveAccessToPCH(PCHLock);
  // need to invalidate everything
  for (auto &I : PCHs)
    I->invalidate(ExclusiveAccessToPCH);

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
      auto Dep =
          std::find_if(PCHsRangeBeg, PCHsRangeEnd, [&](const std::unique_ptr<PCHItem> &XX) {
            return XX->CompileCommand.Filename == DepFName;
          });
      (*Dep)->DependOnMe.push_back(&*(PCHs.back()));
      PCHs.back()->IdependOn.push_back(&(**Dep));
    }

    PrevPartBeg = PartBeg;
    PartBeg = NewPartEnd;
  }
}

unsigned PCHManager::PCHItem::invalidate(uniq_lck &Lock) {
  unsigned Res = 0;
  if (ItemState != PCHItem::State::Rebuild) {
    ++Res;
    if (ItemState == PCHItem::State::Valid && InUse > 0)
      CV.wait(Lock, [&] { return InUse == 0; });

    ItemState = PCHItem::State::Rebuild;
    ++Version;
    PCHData.clear();
    // all dependencies must be invalidated
    for (PCHItem *Dep : DependOnMe)
      Res += Dep->invalidate(Lock);
  }
  return Res;
}

unsigned PCHManager::invalidateAffectedPCH(
    const std::vector<std::string> &ChangedFiles) {
  uniq_lck ExclusiveAccessToPCH(PCHLock);
  unsigned Invalidated = 0;
  llvm::StringSet<> Changed;
  for (const auto &S : ChangedFiles)
    Changed.insert(S);
  for (auto &UI : PCHs) {
    PCHItem &Item = *UI;
    if (Item.ItemState == PCHItem::State::Rebuild)
      continue;

    if (Changed.find(Item.CompileCommand.Filename) != Changed.end()) {
      log("(PCH) invalidating {0} and all dependendents",
          Item.CompileCommand.Filename);
      Invalidated += Item.invalidate(ExclusiveAccessToPCH);
      continue;
    }

    for (const auto &S : Item.Includes.allHeaders()) {
      if (Changed.find(S) != Changed.end()) {
        log("(PCH) invalidating {0} and all dependendents because"
            " of the included (possible indirectly) {1} has changed",
            Item.CompileCommand.Filename, S);
        Invalidated += Item.invalidate(ExclusiveAccessToPCH);
        break;
      }
    }
  }
  return Invalidated;
}

IntrusiveRefCntPtr<llvm::vfs::FileSystem>
PCHManager::addDependencies(const PCHItem *Dep,
                            IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) {
  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> PCHFS(
      new llvm::vfs::InMemoryFileSystem());
  while (Dep) {
    auto Buf = llvm::MemoryBuffer::getMemBuffer(Dep->PCHData);
    PCHFS->addFile(Dep->CompileCommand.Filename + ".pch", 0, std::move(Buf));
    Dep = Dep->IdependOn.empty() ? nullptr : Dep->IdependOn[0];
  }

  IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
      new llvm::vfs::OverlayFileSystem(VFS));

  Overlay->pushOverlay(PCHFS);
  return Overlay;
}

void PCHManager::rebuildPCH(PCHItem &Item, FSType FS) {
  auto S = PCHItem::State::Invalid;
  auto OnExit = llvm::make_scope_exit([&] {
    Item.ItemState = S;
    Item.CV.notify_all();
  });

  PCHItem *Dep = Item.IdependOn.empty() ? nullptr : Item.IdependOn[0];
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

  auto VFS = TFS.view(Item.CompileCommand.Directory);
  if (FS) {
    IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> Overlay(
        new llvm::vfs::OverlayFileSystem(FS)); // passed FS is primary
    Overlay->pushOverlay(VFS);                 // VFS is secondary
    VFS = Overlay;
  }
  if (Dep) {
    PreprocessorOpts.ImplicitPCHInclude =
        std::string(Dep->CompileCommand.Filename) + ".pch";
    VFS = addDependencies(Dep, VFS);
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
  FrontendOpts.OutputFile = "__in__memory___";

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

  std::unique_ptr<PrecompilePCHAction> Act;
  Act.reset(new PrecompilePCHAction(&Item.PCHData, Callbacks));
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
    elog("(PCH)Could not emmit PCH for {0}", Item.CompileCommand.Filename);
    return;
  }

  Item.Includes = SerializedDeclsCollector.takeIncludes();
  Item.CanonIncludes = SerializedDeclsCollector.takeCanonicalIncludes();
  S = PCHItem::State::Valid;
  log("(PCH)Successfully generated precompiled header of size: {0}",
      Item.PCHData.size());
}

void PCHManager::rebuildInvalidatedPCH(unsigned Total, FSType FS) {
  unsigned Complete = 0;
  if (OnProgress)
    OnProgress(Stats{Complete, Total});
  for (auto &I : PCHs) {
    if (I->ItemState == PCHItem::State::Rebuild) {
      rebuildPCH(*I, FS);
      if (OnProgress)
        OnProgress(Stats{++Complete, Total});
    }
  }
}

PCHManager::PCHAccess
PCHManager::tryFindPCH(tooling::CompileCommand const &Cmd) const {
  if (!Initialized && !WaitForInit) {
    log("(findPCH) is not initialized yet. Return empty for {0}", Cmd.Filename);
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

PCHManager::PCHAccess
PCHManager::findPCH(tooling::CompileCommand const &Cmd) const {
  // return {};
  llvm::StringRef Dep = findPCHDependency(Cmd);
  if (!Dep.empty())
    return findPCH(Dep);
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
      if (I->ItemState == PCHItem::State::Rebuild)
        I->CV.wait(Lock, [&] { return I->ItemState != PCHItem::State::Rebuild; });

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
    CI->getPreprocessorOpts().DisablePCHOrModuleValidation =
        DisableValidationForModuleKind::PCH;
    CI->getPreprocessorOpts().ImplicitPCHInclude =
        std::string(Item->CompileCommand.Filename) + ".pch";
    VFS = addDependencies(Item, VFS);
    return true;
  }
  return false;
}

PCHManager::PCHAccess::PCHAccess(const PCHItem *Item) : Item(Item) {
  if (Item)
    ++Item->InUse;
}

PCHManager::PCHAccess::PCHAccess(PCHAccess &&Rhs) : Item(Rhs.Item) {
  Rhs.Item = nullptr;
}
PCHManager::PCHAccess::~PCHAccess() {
  if (Item && (Item->InUse.fetch_sub(1) == 1))
    Item->CV.notify_all();
}

PCHManager::PCHAccess &PCHManager::PCHAccess::operator=(PCHAccess &&Rhs) {
  std::swap(Item, Rhs.Item);
  return *this;
}

} // namespace clangd
} // namespace clang