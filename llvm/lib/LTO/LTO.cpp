//===-LTO.cpp - LLVM Link Time Optimizer ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements functions and classes used to support LTO.
//
//===----------------------------------------------------------------------===//

#include "llvm/LTO/LTO.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StableHashing.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/StackSafetyAnalysis.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/CGData/CodeGenData.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/AutoUpgrade.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMRemarkStreamer.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/RuntimeLibcalls.h"
#include "llvm/LTO/LTOBackend.h"
#include "llvm/Linker/IRMover.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/IRObjectFile.h"
#include "llvm/Support/Caching.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/VCSRevision.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/MemProfContextDisambiguation.h"
#include "llvm/Transforms/IPO/WholeProgramDevirt.h"
#include "llvm/Transforms/Utils/FunctionImportUtils.h"
#include "llvm/Transforms/Utils/SplitModule.h"

#include <optional>
#include <set>

using namespace llvm;
using namespace lto;
using namespace object;

#define DEBUG_TYPE "lto"

static cl::opt<bool>
    DumpThinCGSCCs("dump-thin-cg-sccs", cl::init(false), cl::Hidden,
                   cl::desc("Dump the SCCs in the ThinLTO index's callgraph"));

extern cl::opt<bool> CodeGenDataThinLTOTwoRounds;

extern cl::opt<bool> ForceImportAll;

namespace llvm {
/// Enable global value internalization in LTO.
cl::opt<bool> EnableLTOInternalization(
    "enable-lto-internalization", cl::init(true), cl::Hidden,
    cl::desc("Enable global value internalization in LTO"));

static cl::opt<bool>
    LTOKeepSymbolCopies("lto-keep-symbol-copies", cl::init(false), cl::Hidden,
                        cl::desc("Keep copies of symbols in LTO indexing"));

/// Indicate we are linking with an allocator that supports hot/cold operator
/// new interfaces.
extern cl::opt<bool> SupportsHotColdNew;

/// Enable MemProf context disambiguation for thin link.
extern cl::opt<bool> EnableMemProfContextDisambiguation;
} // namespace llvm

// Computes a unique hash for the Module considering the current list of
// export/import and other global analysis results.
// Returns the hash in its hexadecimal representation.
std::string llvm::computeLTOCacheKey(
    const Config &Conf, const ModuleSummaryIndex &Index, StringRef ModuleID,
    const FunctionImporter::ImportMapTy &ImportList,
    const FunctionImporter::ExportSetTy &ExportList,
    const std::map<GlobalValue::GUID, GlobalValue::LinkageTypes> &ResolvedODR,
    const GVSummaryMapTy &DefinedGlobals,
    const DenseSet<GlobalValue::GUID> &CfiFunctionDefs,
    const DenseSet<GlobalValue::GUID> &CfiFunctionDecls) {
  // Compute the unique hash for this entry.
  // This is based on the current compiler version, the module itself, the
  // export list, the hash for every single module in the import list, the
  // list of ResolvedODR for the module, and the list of preserved symbols.
  SHA1 Hasher;

  // Start with the compiler revision
  Hasher.update(LLVM_VERSION_STRING);
#ifdef LLVM_REVISION
  Hasher.update(LLVM_REVISION);
#endif

  // Include the parts of the LTO configuration that affect code generation.
  auto AddString = [&](StringRef Str) {
    Hasher.update(Str);
    Hasher.update(ArrayRef<uint8_t>{0});
  };
  auto AddUnsigned = [&](unsigned I) {
    uint8_t Data[4];
    support::endian::write32le(Data, I);
    Hasher.update(Data);
  };
  auto AddUint64 = [&](uint64_t I) {
    uint8_t Data[8];
    support::endian::write64le(Data, I);
    Hasher.update(Data);
  };
  auto AddUint8 = [&](const uint8_t I) {
    Hasher.update(ArrayRef<uint8_t>(&I, 1));
  };
  AddString(Conf.CPU);
  // FIXME: Hash more of Options. For now all clients initialize Options from
  // command-line flags (which is unsupported in production), but may set
  // X86RelaxRelocations. The clang driver can also pass FunctionSections,
  // DataSections and DebuggerTuning via command line flags.
  AddUnsigned(Conf.Options.MCOptions.X86RelaxRelocations);
  AddUnsigned(Conf.Options.FunctionSections);
  AddUnsigned(Conf.Options.DataSections);
  AddUnsigned((unsigned)Conf.Options.DebuggerTuning);
  for (auto &A : Conf.MAttrs)
    AddString(A);
  if (Conf.RelocModel)
    AddUnsigned(*Conf.RelocModel);
  else
    AddUnsigned(-1);
  if (Conf.CodeModel)
    AddUnsigned(*Conf.CodeModel);
  else
    AddUnsigned(-1);
  for (const auto &S : Conf.MllvmArgs)
    AddString(S);
  AddUnsigned(static_cast<int>(Conf.CGOptLevel));
  AddUnsigned(static_cast<int>(Conf.CGFileType));
  AddUnsigned(Conf.OptLevel);
  AddUnsigned(Conf.Freestanding);
  AddString(Conf.OptPipeline);
  AddString(Conf.AAPipeline);
  AddString(Conf.OverrideTriple);
  AddString(Conf.DefaultTriple);
  AddString(Conf.DwoDir);

  // Include the hash for the current module
  auto ModHash = Index.getModuleHash(ModuleID);
  Hasher.update(ArrayRef<uint8_t>((uint8_t *)&ModHash[0], sizeof(ModHash)));

  // TODO: `ExportList` is determined by `ImportList`. Since `ImportList` is
  // used to compute cache key, we could omit hashing `ExportList` here.
  std::vector<uint64_t> ExportsGUID;
  ExportsGUID.reserve(ExportList.size());
  for (const auto &VI : ExportList)
    ExportsGUID.push_back(VI.getGUID());

  // Sort the export list elements GUIDs.
  llvm::sort(ExportsGUID);
  for (auto GUID : ExportsGUID)
    Hasher.update(ArrayRef<uint8_t>((uint8_t *)&GUID, sizeof(GUID)));

  // Order using module hash, to be both independent of module name and
  // module order.
  auto Comp = [&](const std::pair<StringRef, GlobalValue::GUID> &L,
                  const std::pair<StringRef, GlobalValue::GUID> &R) {
    return std::make_pair(Index.getModule(L.first)->second, L.second) <
           std::make_pair(Index.getModule(R.first)->second, R.second);
  };
  FunctionImporter::SortedImportList SortedImportList(ImportList, Comp);

  // Count the number of imports for each source module.
  DenseMap<StringRef, unsigned> ModuleToNumImports;
  for (const auto &[FromModule, GUID, Type] : SortedImportList)
    ++ModuleToNumImports[FromModule];

  std::optional<StringRef> LastModule;
  for (const auto &[FromModule, GUID, Type] : SortedImportList) {
    if (LastModule != FromModule) {
      // Include the hash for every module we import functions from. The set of
      // imported symbols for each module may affect code generation and is
      // sensitive to link order, so include that as well.
      LastModule = FromModule;
      auto ModHash = Index.getModule(FromModule)->second;
      Hasher.update(ArrayRef<uint8_t>((uint8_t *)&ModHash[0], sizeof(ModHash)));
      AddUint64(ModuleToNumImports[FromModule]);
    }
    AddUint64(GUID);
    AddUint8(Type);
  }

  // Include the hash for the resolved ODR.
  for (auto &Entry : ResolvedODR) {
    Hasher.update(ArrayRef<uint8_t>((const uint8_t *)&Entry.first,
                                    sizeof(GlobalValue::GUID)));
    Hasher.update(ArrayRef<uint8_t>((const uint8_t *)&Entry.second,
                                    sizeof(GlobalValue::LinkageTypes)));
  }

  // Members of CfiFunctionDefs and CfiFunctionDecls that are referenced or
  // defined in this module.
  std::set<GlobalValue::GUID> UsedCfiDefs;
  std::set<GlobalValue::GUID> UsedCfiDecls;

  // Typeids used in this module.
  std::set<GlobalValue::GUID> UsedTypeIds;

  auto AddUsedCfiGlobal = [&](GlobalValue::GUID ValueGUID) {
    if (CfiFunctionDefs.contains(ValueGUID))
      UsedCfiDefs.insert(ValueGUID);
    if (CfiFunctionDecls.contains(ValueGUID))
      UsedCfiDecls.insert(ValueGUID);
  };

  auto AddUsedThings = [&](GlobalValueSummary *GS) {
    if (!GS) return;
    AddUnsigned(GS->getVisibility());
    AddUnsigned(GS->isLive());
    AddUnsigned(GS->canAutoHide());
    for (const ValueInfo &VI : GS->refs()) {
      AddUnsigned(VI.isDSOLocal(Index.withDSOLocalPropagation()));
      AddUsedCfiGlobal(VI.getGUID());
    }
    if (auto *GVS = dyn_cast<GlobalVarSummary>(GS)) {
      AddUnsigned(GVS->maybeReadOnly());
      AddUnsigned(GVS->maybeWriteOnly());
    }
    if (auto *FS = dyn_cast<FunctionSummary>(GS)) {
      for (auto &TT : FS->type_tests())
        UsedTypeIds.insert(TT);
      for (auto &TT : FS->type_test_assume_vcalls())
        UsedTypeIds.insert(TT.GUID);
      for (auto &TT : FS->type_checked_load_vcalls())
        UsedTypeIds.insert(TT.GUID);
      for (auto &TT : FS->type_test_assume_const_vcalls())
        UsedTypeIds.insert(TT.VFunc.GUID);
      for (auto &TT : FS->type_checked_load_const_vcalls())
        UsedTypeIds.insert(TT.VFunc.GUID);
      for (auto &ET : FS->calls()) {
        AddUnsigned(ET.first.isDSOLocal(Index.withDSOLocalPropagation()));
        AddUsedCfiGlobal(ET.first.getGUID());
      }
    }
  };

  // Include the hash for the linkage type to reflect internalization and weak
  // resolution, and collect any used type identifier resolutions.
  for (auto &GS : DefinedGlobals) {
    GlobalValue::LinkageTypes Linkage = GS.second->linkage();
    Hasher.update(
        ArrayRef<uint8_t>((const uint8_t *)&Linkage, sizeof(Linkage)));
    AddUsedCfiGlobal(GS.first);
    AddUsedThings(GS.second);
  }

  // Imported functions may introduce new uses of type identifier resolutions,
  // so we need to collect their used resolutions as well.
  for (const auto &[FromModule, GUID, Type] : SortedImportList) {
    GlobalValueSummary *S = Index.findSummaryInModule(GUID, FromModule);
    AddUsedThings(S);
    // If this is an alias, we also care about any types/etc. that the aliasee
    // may reference.
    if (auto *AS = dyn_cast_or_null<AliasSummary>(S))
      AddUsedThings(AS->getBaseObject());
  }

  auto AddTypeIdSummary = [&](StringRef TId, const TypeIdSummary &S) {
    AddString(TId);

    AddUnsigned(S.TTRes.TheKind);
    AddUnsigned(S.TTRes.SizeM1BitWidth);

    AddUint64(S.TTRes.AlignLog2);
    AddUint64(S.TTRes.SizeM1);
    AddUint64(S.TTRes.BitMask);
    AddUint64(S.TTRes.InlineBits);

    AddUint64(S.WPDRes.size());
    for (auto &WPD : S.WPDRes) {
      AddUnsigned(WPD.first);
      AddUnsigned(WPD.second.TheKind);
      AddString(WPD.second.SingleImplName);

      AddUint64(WPD.second.ResByArg.size());
      for (auto &ByArg : WPD.second.ResByArg) {
        AddUint64(ByArg.first.size());
        for (uint64_t Arg : ByArg.first)
          AddUint64(Arg);
        AddUnsigned(ByArg.second.TheKind);
        AddUint64(ByArg.second.Info);
        AddUnsigned(ByArg.second.Byte);
        AddUnsigned(ByArg.second.Bit);
      }
    }
  };

  // Include the hash for all type identifiers used by this module.
  for (GlobalValue::GUID TId : UsedTypeIds) {
    auto TidIter = Index.typeIds().equal_range(TId);
    for (const auto &I : make_range(TidIter))
      AddTypeIdSummary(I.second.first, I.second.second);
  }

  AddUnsigned(UsedCfiDefs.size());
  for (auto &V : UsedCfiDefs)
    AddUint64(V);

  AddUnsigned(UsedCfiDecls.size());
  for (auto &V : UsedCfiDecls)
    AddUint64(V);

  if (!Conf.SampleProfile.empty()) {
    auto FileOrErr = MemoryBuffer::getFile(Conf.SampleProfile);
    if (FileOrErr) {
      Hasher.update(FileOrErr.get()->getBuffer());

      if (!Conf.ProfileRemapping.empty()) {
        FileOrErr = MemoryBuffer::getFile(Conf.ProfileRemapping);
        if (FileOrErr)
          Hasher.update(FileOrErr.get()->getBuffer());
      }
    }
  }

  return toHex(Hasher.result());
}

std::string llvm::recomputeLTOCacheKey(const std::string &Key,
                                       StringRef ExtraID) {
  SHA1 Hasher;

  auto AddString = [&](StringRef Str) {
    Hasher.update(Str);
    Hasher.update(ArrayRef<uint8_t>{0});
  };
  AddString(Key);
  AddString(ExtraID);

  return toHex(Hasher.result());
}

static void thinLTOResolvePrevailingGUID(
    const Config &C, ValueInfo VI,
    DenseSet<GlobalValueSummary *> &GlobalInvolvedWithAlias,
    function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
        isPrevailing,
    function_ref<void(StringRef, GlobalValue::GUID, GlobalValue::LinkageTypes)>
        recordNewLinkage,
    const DenseSet<GlobalValue::GUID> &GUIDPreservedSymbols) {
  GlobalValue::VisibilityTypes Visibility =
      C.VisibilityScheme == Config::ELF ? VI.getELFVisibility()
                                        : GlobalValue::DefaultVisibility;
  for (auto &S : VI.getSummaryList()) {
    GlobalValue::LinkageTypes OriginalLinkage = S->linkage();
    // Ignore local and appending linkage values since the linker
    // doesn't resolve them.
    if (GlobalValue::isLocalLinkage(OriginalLinkage) ||
        GlobalValue::isAppendingLinkage(S->linkage()))
      continue;
    // We need to emit only one of these. The prevailing module will keep it,
    // but turned into a weak, while the others will drop it when possible.
    // This is both a compile-time optimization and a correctness
    // transformation. This is necessary for correctness when we have exported
    // a reference - we need to convert the linkonce to weak to
    // ensure a copy is kept to satisfy the exported reference.
    // FIXME: We may want to split the compile time and correctness
    // aspects into separate routines.
    if (isPrevailing(VI.getGUID(), S.get())) {
      if (GlobalValue::isLinkOnceLinkage(OriginalLinkage)) {
        S->setLinkage(GlobalValue::getWeakLinkage(
            GlobalValue::isLinkOnceODRLinkage(OriginalLinkage)));
        // The kept copy is eligible for auto-hiding (hidden visibility) if all
        // copies were (i.e. they were all linkonce_odr global unnamed addr).
        // If any copy is not (e.g. it was originally weak_odr), then the symbol
        // must remain externally available (e.g. a weak_odr from an explicitly
        // instantiated template). Additionally, if it is in the
        // GUIDPreservedSymbols set, that means that it is visibile outside
        // the summary (e.g. in a native object or a bitcode file without
        // summary), and in that case we cannot hide it as it isn't possible to
        // check all copies.
        S->setCanAutoHide(VI.canAutoHide() &&
                          !GUIDPreservedSymbols.count(VI.getGUID()));
      }
      if (C.VisibilityScheme == Config::FromPrevailing)
        Visibility = S->getVisibility();
    }
    // Alias and aliasee can't be turned into available_externally.
    // When force-import-all is used, it indicates that object linking is not
    // supported by the target. In this case, we can't change the linkage as
    // well in case the global is converted to declaration.
    else if (!isa<AliasSummary>(S.get()) &&
             !GlobalInvolvedWithAlias.count(S.get()) && !ForceImportAll)
      S->setLinkage(GlobalValue::AvailableExternallyLinkage);

    // For ELF, set visibility to the computed visibility from summaries. We
    // don't track visibility from declarations so this may be more relaxed than
    // the most constraining one.
    if (C.VisibilityScheme == Config::ELF)
      S->setVisibility(Visibility);

    if (S->linkage() != OriginalLinkage)
      recordNewLinkage(S->modulePath(), VI.getGUID(), S->linkage());
  }

  if (C.VisibilityScheme == Config::FromPrevailing) {
    for (auto &S : VI.getSummaryList()) {
      GlobalValue::LinkageTypes OriginalLinkage = S->linkage();
      if (GlobalValue::isLocalLinkage(OriginalLinkage) ||
          GlobalValue::isAppendingLinkage(S->linkage()))
        continue;
      S->setVisibility(Visibility);
    }
  }
}

/// Resolve linkage for prevailing symbols in the \p Index.
//
// We'd like to drop these functions if they are no longer referenced in the
// current module. However there is a chance that another module is still
// referencing them because of the import. We make sure we always emit at least
// one copy.
void llvm::thinLTOResolvePrevailingInIndex(
    const Config &C, ModuleSummaryIndex &Index,
    function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
        isPrevailing,
    function_ref<void(StringRef, GlobalValue::GUID, GlobalValue::LinkageTypes)>
        recordNewLinkage,
    const DenseSet<GlobalValue::GUID> &GUIDPreservedSymbols) {
  // We won't optimize the globals that are referenced by an alias for now
  // Ideally we should turn the alias into a global and duplicate the definition
  // when needed.
  DenseSet<GlobalValueSummary *> GlobalInvolvedWithAlias;
  for (auto &I : Index)
    for (auto &S : I.second.SummaryList)
      if (auto AS = dyn_cast<AliasSummary>(S.get()))
        GlobalInvolvedWithAlias.insert(&AS->getAliasee());

  for (auto &I : Index)
    thinLTOResolvePrevailingGUID(C, Index.getValueInfo(I),
                                 GlobalInvolvedWithAlias, isPrevailing,
                                 recordNewLinkage, GUIDPreservedSymbols);
}

static void thinLTOInternalizeAndPromoteGUID(
    ValueInfo VI, function_ref<bool(StringRef, ValueInfo)> isExported,
    function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
        isPrevailing) {
  auto ExternallyVisibleCopies =
      llvm::count_if(VI.getSummaryList(),
                     [](const std::unique_ptr<GlobalValueSummary> &Summary) {
                       return !GlobalValue::isLocalLinkage(Summary->linkage());
                     });

  for (auto &S : VI.getSummaryList()) {
    // First see if we need to promote an internal value because it is not
    // exported.
    if (isExported(S->modulePath(), VI)) {
      if (GlobalValue::isLocalLinkage(S->linkage()))
        S->setLinkage(GlobalValue::ExternalLinkage);
      continue;
    }

    // Otherwise, see if we can internalize.
    if (!EnableLTOInternalization)
      continue;

    // Non-exported values with external linkage can be internalized.
    if (GlobalValue::isExternalLinkage(S->linkage())) {
      S->setLinkage(GlobalValue::InternalLinkage);
      continue;
    }

    // Non-exported function and variable definitions with a weak-for-linker
    // linkage can be internalized in certain cases. The minimum legality
    // requirements would be that they are not address taken to ensure that we
    // don't break pointer equality checks, and that variables are either read-
    // or write-only. For functions, this is the case if either all copies are
    // [local_]unnamed_addr, or we can propagate reference edge attributes
    // (which is how this is guaranteed for variables, when analyzing whether
    // they are read or write-only).
    //
    // However, we only get to this code for weak-for-linkage values in one of
    // two cases:
    // 1) The prevailing copy is not in IR (it is in native code).
    // 2) The prevailing copy in IR is not exported from its module.
    // Additionally, at least for the new LTO API, case 2 will only happen if
    // there is exactly one definition of the value (i.e. in exactly one
    // module), as duplicate defs are result in the value being marked exported.
    // Likely, users of the legacy LTO API are similar, however, currently there
    // are llvm-lto based tests of the legacy LTO API that do not mark
    // duplicate linkonce_odr copies as exported via the tool, so we need
    // to handle that case below by checking the number of copies.
    //
    // Generally, we only want to internalize a weak-for-linker value in case
    // 2, because in case 1 we cannot see how the value is used to know if it
    // is read or write-only. We also don't want to bloat the binary with
    // multiple internalized copies of non-prevailing linkonce/weak functions.
    // Note if we don't internalize, we will convert non-prevailing copies to
    // available_externally anyway, so that we drop them after inlining. The
    // only reason to internalize such a function is if we indeed have a single
    // copy, because internalizing it won't increase binary size, and enables
    // use of inliner heuristics that are more aggressive in the face of a
    // single call to a static (local). For variables, internalizing a read or
    // write only variable can enable more aggressive optimization. However, we
    // already perform this elsewhere in the ThinLTO backend handling for
    // read or write-only variables (processGlobalForThinLTO).
    //
    // Therefore, only internalize linkonce/weak if there is a single copy, that
    // is prevailing in this IR module. We can do so aggressively, without
    // requiring the address to be insignificant, or that a variable be read or
    // write-only.
    if (!GlobalValue::isWeakForLinker(S->linkage()) ||
        GlobalValue::isExternalWeakLinkage(S->linkage()))
      continue;

    if (isPrevailing(VI.getGUID(), S.get()) && ExternallyVisibleCopies == 1)
      S->setLinkage(GlobalValue::InternalLinkage);
  }
}

// Update the linkages in the given \p Index to mark exported values
// as external and non-exported values as internal.
void llvm::thinLTOInternalizeAndPromoteInIndex(
    ModuleSummaryIndex &Index,
    function_ref<bool(StringRef, ValueInfo)> isExported,
    function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
        isPrevailing) {
  for (auto &I : Index)
    thinLTOInternalizeAndPromoteGUID(Index.getValueInfo(I), isExported,
                                     isPrevailing);
}

// Requires a destructor for std::vector<InputModule>.
InputFile::~InputFile() = default;

Expected<std::unique_ptr<InputFile>> InputFile::create(MemoryBufferRef Object) {
  std::unique_ptr<InputFile> File(new InputFile);

  Expected<IRSymtabFile> FOrErr = readIRSymtab(Object);
  if (!FOrErr)
    return FOrErr.takeError();

  File->TargetTriple = FOrErr->TheReader.getTargetTriple();
  File->SourceFileName = FOrErr->TheReader.getSourceFileName();
  File->COFFLinkerOpts = FOrErr->TheReader.getCOFFLinkerOpts();
  File->DependentLibraries = FOrErr->TheReader.getDependentLibraries();
  File->ComdatTable = FOrErr->TheReader.getComdatTable();

  for (unsigned I = 0; I != FOrErr->Mods.size(); ++I) {
    size_t Begin = File->Symbols.size();
    for (const irsymtab::Reader::SymbolRef &Sym :
         FOrErr->TheReader.module_symbols(I))
      // Skip symbols that are irrelevant to LTO. Note that this condition needs
      // to match the one in Skip() in LTO::addRegularLTO().
      if (Sym.isGlobal() && !Sym.isFormatSpecific())
        File->Symbols.push_back(Sym);
    File->ModuleSymIndices.push_back({Begin, File->Symbols.size()});
  }

  File->Mods = FOrErr->Mods;
  File->Strtab = std::move(FOrErr->Strtab);
  return std::move(File);
}

StringRef InputFile::getName() const {
  return Mods[0].getModuleIdentifier();
}

BitcodeModule &InputFile::getSingleBitcodeModule() {
  assert(Mods.size() == 1 && "Expect only one bitcode module");
  return Mods[0];
}

LTO::RegularLTOState::RegularLTOState(unsigned ParallelCodeGenParallelismLevel,
                                      const Config &Conf)
    : ParallelCodeGenParallelismLevel(ParallelCodeGenParallelismLevel),
      Ctx(Conf), CombinedModule(std::make_unique<Module>("ld-temp.o", Ctx)),
      Mover(std::make_unique<IRMover>(*CombinedModule)) {}

LTO::ThinLTOState::ThinLTOState(ThinBackend BackendParam)
    : Backend(std::move(BackendParam)), CombinedIndex(/*HaveGVs*/ false) {
  if (!Backend.isValid())
    Backend =
        createInProcessThinBackend(llvm::heavyweight_hardware_concurrency());
}

LTO::LTO(Config Conf, ThinBackend Backend,
         unsigned ParallelCodeGenParallelismLevel, LTOKind LTOMode)
    : Conf(std::move(Conf)),
      RegularLTO(ParallelCodeGenParallelismLevel, this->Conf),
      ThinLTO(std::move(Backend)),
      GlobalResolutions(
          std::make_unique<DenseMap<StringRef, GlobalResolution>>()),
      LTOMode(LTOMode) {
  if (Conf.KeepSymbolNameCopies || LTOKeepSymbolCopies) {
    Alloc = std::make_unique<BumpPtrAllocator>();
    GlobalResolutionSymbolSaver = std::make_unique<llvm::StringSaver>(*Alloc);
  }
}

// Requires a destructor for MapVector<BitcodeModule>.
LTO::~LTO() = default;

// Add the symbols in the given module to the GlobalResolutions map, and resolve
// their partitions.
void LTO::addModuleToGlobalRes(ArrayRef<InputFile::Symbol> Syms,
                               ArrayRef<SymbolResolution> Res,
                               unsigned Partition, bool InSummary) {
  auto *ResI = Res.begin();
  auto *ResE = Res.end();
  (void)ResE;
  for (const InputFile::Symbol &Sym : Syms) {
    assert(ResI != ResE);
    SymbolResolution Res = *ResI++;

    StringRef SymbolName = Sym.getName();
    // Keep copies of symbols if the client of LTO says so.
    if (GlobalResolutionSymbolSaver && !GlobalResolutions->contains(SymbolName))
      SymbolName = GlobalResolutionSymbolSaver->save(SymbolName);

    auto &GlobalRes = (*GlobalResolutions)[SymbolName];
    GlobalRes.UnnamedAddr &= Sym.isUnnamedAddr();
    if (Res.Prevailing) {
      assert(!GlobalRes.Prevailing &&
             "Multiple prevailing defs are not allowed");
      GlobalRes.Prevailing = true;
      GlobalRes.IRName = std::string(Sym.getIRName());
    } else if (!GlobalRes.Prevailing && GlobalRes.IRName.empty()) {
      // Sometimes it can be two copies of symbol in a module and prevailing
      // symbol can have no IR name. That might happen if symbol is defined in
      // module level inline asm block. In case we have multiple modules with
      // the same symbol we want to use IR name of the prevailing symbol.
      // Otherwise, if we haven't seen a prevailing symbol, set the name so that
      // we can later use it to check if there is any prevailing copy in IR.
      GlobalRes.IRName = std::string(Sym.getIRName());
    }

    // In rare occasion, the symbol used to initialize GlobalRes has a different
    // IRName from the inspected Symbol. This can happen on macOS + iOS, when a
    // symbol is referenced through its mangled name, say @"\01_symbol" while
    // the IRName is @symbol (the prefix underscore comes from MachO mangling).
    // In that case, we have the same actual Symbol that can get two different
    // GUID, leading to some invalid internalization. Workaround this by marking
    // the GlobalRes external.

    // FIXME: instead of this check, it would be desirable to compute GUIDs
    // based on mangled name, but this requires an access to the Target Triple
    // and would be relatively invasive on the codebase.
    if (GlobalRes.IRName != Sym.getIRName()) {
      GlobalRes.Partition = GlobalResolution::External;
      GlobalRes.VisibleOutsideSummary = true;
    }

    // Set the partition to external if we know it is re-defined by the linker
    // with -defsym or -wrap options, used elsewhere, e.g. it is visible to a
    // regular object, is referenced from llvm.compiler.used/llvm.used, or was
    // already recorded as being referenced from a different partition.
    if (Res.LinkerRedefined || Res.VisibleToRegularObj || Sym.isUsed() ||
        (GlobalRes.Partition != GlobalResolution::Unknown &&
         GlobalRes.Partition != Partition)) {
      GlobalRes.Partition = GlobalResolution::External;
    } else
      // First recorded reference, save the current partition.
      GlobalRes.Partition = Partition;

    // Flag as visible outside of summary if visible from a regular object or
    // from a module that does not have a summary.
    GlobalRes.VisibleOutsideSummary |=
        (Res.VisibleToRegularObj || Sym.isUsed() || !InSummary);

    GlobalRes.ExportDynamic |= Res.ExportDynamic;
  }
}

void LTO::releaseGlobalResolutionsMemory() {
  // Release GlobalResolutions dense-map itself.
  GlobalResolutions.reset();
  // Release the string saver memory.
  GlobalResolutionSymbolSaver.reset();
  Alloc.reset();
}

static void writeToResolutionFile(raw_ostream &OS, InputFile *Input,
                                  ArrayRef<SymbolResolution> Res) {
  StringRef Path = Input->getName();
  OS << Path << '\n';
  auto ResI = Res.begin();
  for (const InputFile::Symbol &Sym : Input->symbols()) {
    assert(ResI != Res.end());
    SymbolResolution Res = *ResI++;

    OS << "-r=" << Path << ',' << Sym.getName() << ',';
    if (Res.Prevailing)
      OS << 'p';
    if (Res.FinalDefinitionInLinkageUnit)
      OS << 'l';
    if (Res.VisibleToRegularObj)
      OS << 'x';
    if (Res.LinkerRedefined)
      OS << 'r';
    OS << '\n';
  }
  OS.flush();
  assert(ResI == Res.end());
}

Error LTO::add(std::unique_ptr<InputFile> Input,
               ArrayRef<SymbolResolution> Res) {
  assert(!CalledGetMaxTasks);

  if (Conf.ResolutionFile)
    writeToResolutionFile(*Conf.ResolutionFile, Input.get(), Res);

  if (RegularLTO.CombinedModule->getTargetTriple().empty()) {
    Triple InputTriple(Input->getTargetTriple());
    RegularLTO.CombinedModule->setTargetTriple(InputTriple);
    if (InputTriple.isOSBinFormatELF())
      Conf.VisibilityScheme = Config::ELF;
  }

  ArrayRef<SymbolResolution> InputRes = Res;
  for (unsigned I = 0; I != Input->Mods.size(); ++I) {
    if (auto Err = addModule(*Input, InputRes, I, Res).moveInto(Res))
      return Err;
  }

  assert(Res.empty());
  return Error::success();
}

Expected<ArrayRef<SymbolResolution>>
LTO::addModule(InputFile &Input, ArrayRef<SymbolResolution> InputRes,
               unsigned ModI, ArrayRef<SymbolResolution> Res) {
  Expected<BitcodeLTOInfo> LTOInfo = Input.Mods[ModI].getLTOInfo();
  if (!LTOInfo)
    return LTOInfo.takeError();

  if (EnableSplitLTOUnit) {
    // If only some modules were split, flag this in the index so that
    // we can skip or error on optimizations that need consistently split
    // modules (whole program devirt and lower type tests).
    if (*EnableSplitLTOUnit != LTOInfo->EnableSplitLTOUnit)
      ThinLTO.CombinedIndex.setPartiallySplitLTOUnits();
  } else
    EnableSplitLTOUnit = LTOInfo->EnableSplitLTOUnit;

  BitcodeModule BM = Input.Mods[ModI];

  if ((LTOMode == LTOK_UnifiedRegular || LTOMode == LTOK_UnifiedThin) &&
      !LTOInfo->UnifiedLTO)
    return make_error<StringError>(
        "unified LTO compilation must use "
        "compatible bitcode modules (use -funified-lto)",
        inconvertibleErrorCode());

  if (LTOInfo->UnifiedLTO && LTOMode == LTOK_Default)
    LTOMode = LTOK_UnifiedThin;

  bool IsThinLTO = LTOInfo->IsThinLTO && (LTOMode != LTOK_UnifiedRegular);

  auto ModSyms = Input.module_symbols(ModI);
  addModuleToGlobalRes(ModSyms, Res,
                       IsThinLTO ? ThinLTO.ModuleMap.size() + 1 : 0,
                       LTOInfo->HasSummary);

  if (IsThinLTO)
    return addThinLTO(BM, ModSyms, Res);

  RegularLTO.EmptyCombinedModule = false;
  auto ModOrErr = addRegularLTO(Input, InputRes, BM, ModSyms, Res);
  if (!ModOrErr)
    return ModOrErr.takeError();
  Res = ModOrErr->second;

  if (!LTOInfo->HasSummary) {
    if (Error Err = linkRegularLTO(std::move(ModOrErr->first),
                                   /*LivenessFromIndex=*/false))
      return Err;
    return Res;
  }

  // Regular LTO module summaries are added to a dummy module that represents
  // the combined regular LTO module.
  if (Error Err = BM.readSummary(ThinLTO.CombinedIndex, ""))
    return Err;
  RegularLTO.ModsWithSummaries.push_back(std::move(ModOrErr->first));
  return Res;
}

// Checks whether the given global value is in a non-prevailing comdat
// (comdat containing values the linker indicated were not prevailing,
// which we then dropped to available_externally), and if so, removes
// it from the comdat. This is called for all global values to ensure the
// comdat is empty rather than leaving an incomplete comdat. It is needed for
// regular LTO modules, in case we are in a mixed-LTO mode (both regular
// and thin LTO modules) compilation. Since the regular LTO module will be
// linked first in the final native link, we want to make sure the linker
// doesn't select any of these incomplete comdats that would be left
// in the regular LTO module without this cleanup.
static void
handleNonPrevailingComdat(GlobalValue &GV,
                          std::set<const Comdat *> &NonPrevailingComdats) {
  Comdat *C = GV.getComdat();
  if (!C)
    return;

  if (!NonPrevailingComdats.count(C))
    return;

  // Additionally need to drop all global values from the comdat to
  // available_externally, to satisfy the COMDAT requirement that all members
  // are discarded as a unit. The non-local linkage global values avoid
  // duplicate definition linker errors.
  GV.setLinkage(GlobalValue::AvailableExternallyLinkage);

  if (auto GO = dyn_cast<GlobalObject>(&GV))
    GO->setComdat(nullptr);
}

// Add a regular LTO object to the link.
// The resulting module needs to be linked into the combined LTO module with
// linkRegularLTO.
Expected<
    std::pair<LTO::RegularLTOState::AddedModule, ArrayRef<SymbolResolution>>>
LTO::addRegularLTO(InputFile &Input, ArrayRef<SymbolResolution> InputRes,
                   BitcodeModule BM, ArrayRef<InputFile::Symbol> Syms,
                   ArrayRef<SymbolResolution> Res) {
  RegularLTOState::AddedModule Mod;
  Expected<std::unique_ptr<Module>> MOrErr =
      BM.getLazyModule(RegularLTO.Ctx, /*ShouldLazyLoadMetadata*/ true,
                       /*IsImporting*/ false);
  if (!MOrErr)
    return MOrErr.takeError();
  Module &M = **MOrErr;
  Mod.M = std::move(*MOrErr);

  if (Error Err = M.materializeMetadata())
    return std::move(Err);

  if (LTOMode == LTOK_UnifiedRegular) {
    // cfi.functions metadata is intended to be used with ThinLTO and may
    // trigger invalid IR transformations if they are present when doing regular
    // LTO, so delete it.
    if (NamedMDNode *CfiFunctionsMD = M.getNamedMetadata("cfi.functions"))
      M.eraseNamedMetadata(CfiFunctionsMD);
  } else if (NamedMDNode *AliasesMD = M.getNamedMetadata("aliases")) {
    // Delete aliases entries for non-prevailing symbols on the ThinLTO side of
    // this input file.
    DenseSet<StringRef> Prevailing;
    for (auto [I, R] : zip(Input.symbols(), InputRes))
      if (R.Prevailing && !I.getIRName().empty())
        Prevailing.insert(I.getIRName());
    std::vector<MDNode *> AliasGroups;
    for (MDNode *AliasGroup : AliasesMD->operands()) {
      std::vector<Metadata *> Aliases;
      for (Metadata *Alias : AliasGroup->operands()) {
        if (isa<MDString>(Alias) &&
            Prevailing.count(cast<MDString>(Alias)->getString()))
          Aliases.push_back(Alias);
      }
      if (Aliases.size() > 1)
        AliasGroups.push_back(MDTuple::get(RegularLTO.Ctx, Aliases));
    }
    AliasesMD->clearOperands();
    for (MDNode *G : AliasGroups)
      AliasesMD->addOperand(G);
  }

  UpgradeDebugInfo(M);

  ModuleSymbolTable SymTab;
  SymTab.addModule(&M);

  for (GlobalVariable &GV : M.globals())
    if (GV.hasAppendingLinkage())
      Mod.Keep.push_back(&GV);

  DenseSet<GlobalObject *> AliasedGlobals;
  for (auto &GA : M.aliases())
    if (GlobalObject *GO = GA.getAliaseeObject())
      AliasedGlobals.insert(GO);

  // In this function we need IR GlobalValues matching the symbols in Syms
  // (which is not backed by a module), so we need to enumerate them in the same
  // order. The symbol enumeration order of a ModuleSymbolTable intentionally
  // matches the order of an irsymtab, but when we read the irsymtab in
  // InputFile::create we omit some symbols that are irrelevant to LTO. The
  // Skip() function skips the same symbols from the module as InputFile does
  // from the symbol table.
  auto MsymI = SymTab.symbols().begin(), MsymE = SymTab.symbols().end();
  auto Skip = [&]() {
    while (MsymI != MsymE) {
      auto Flags = SymTab.getSymbolFlags(*MsymI);
      if ((Flags & object::BasicSymbolRef::SF_Global) &&
          !(Flags & object::BasicSymbolRef::SF_FormatSpecific))
        return;
      ++MsymI;
    }
  };
  Skip();

  std::set<const Comdat *> NonPrevailingComdats;
  SmallSet<StringRef, 2> NonPrevailingAsmSymbols;
  for (const InputFile::Symbol &Sym : Syms) {
    assert(!Res.empty());
    const SymbolResolution &R = Res.consume_front();

    assert(MsymI != MsymE);
    ModuleSymbolTable::Symbol Msym = *MsymI++;
    Skip();

    if (GlobalValue *GV = dyn_cast_if_present<GlobalValue *>(Msym)) {
      if (R.Prevailing) {
        if (Sym.isUndefined())
          continue;
        Mod.Keep.push_back(GV);
        // For symbols re-defined with linker -wrap and -defsym options,
        // set the linkage to weak to inhibit IPO. The linkage will be
        // restored by the linker.
        if (R.LinkerRedefined)
          GV->setLinkage(GlobalValue::WeakAnyLinkage);

        GlobalValue::LinkageTypes OriginalLinkage = GV->getLinkage();
        if (GlobalValue::isLinkOnceLinkage(OriginalLinkage))
          GV->setLinkage(GlobalValue::getWeakLinkage(
              GlobalValue::isLinkOnceODRLinkage(OriginalLinkage)));
      } else if (isa<GlobalObject>(GV) &&
                 (GV->hasLinkOnceODRLinkage() || GV->hasWeakODRLinkage() ||
                  GV->hasAvailableExternallyLinkage()) &&
                 !AliasedGlobals.count(cast<GlobalObject>(GV))) {
        // Any of the above three types of linkage indicates that the
        // chosen prevailing symbol will have the same semantics as this copy of
        // the symbol, so we may be able to link it with available_externally
        // linkage. We will decide later whether to do that when we link this
        // module (in linkRegularLTO), based on whether it is undefined.
        Mod.Keep.push_back(GV);
        GV->setLinkage(GlobalValue::AvailableExternallyLinkage);
        if (GV->hasComdat())
          NonPrevailingComdats.insert(GV->getComdat());
        cast<GlobalObject>(GV)->setComdat(nullptr);
      }

      // Set the 'local' flag based on the linker resolution for this symbol.
      if (R.FinalDefinitionInLinkageUnit) {
        GV->setDSOLocal(true);
        if (GV->hasDLLImportStorageClass())
          GV->setDLLStorageClass(GlobalValue::DLLStorageClassTypes::
                                 DefaultStorageClass);
      }
    } else if (auto *AS =
                   dyn_cast_if_present<ModuleSymbolTable::AsmSymbol *>(Msym)) {
      // Collect non-prevailing symbols.
      if (!R.Prevailing)
        NonPrevailingAsmSymbols.insert(AS->first);
    } else {
      llvm_unreachable("unknown symbol type");
    }

    // Common resolution: collect the maximum size/alignment over all commons.
    // We also record if we see an instance of a common as prevailing, so that
    // if none is prevailing we can ignore it later.
    if (Sym.isCommon()) {
      // FIXME: We should figure out what to do about commons defined by asm.
      // For now they aren't reported correctly by ModuleSymbolTable.
      auto &CommonRes = RegularLTO.Commons[std::string(Sym.getIRName())];
      CommonRes.Size = std::max(CommonRes.Size, Sym.getCommonSize());
      if (uint32_t SymAlignValue = Sym.getCommonAlignment()) {
        CommonRes.Alignment =
            std::max(Align(SymAlignValue), CommonRes.Alignment);
      }
      CommonRes.Prevailing |= R.Prevailing;
    }
  }

  if (!M.getComdatSymbolTable().empty())
    for (GlobalValue &GV : M.global_values())
      handleNonPrevailingComdat(GV, NonPrevailingComdats);

  // Prepend ".lto_discard <sym>, <sym>*" directive to each module inline asm
  // block.
  if (!M.getModuleInlineAsm().empty()) {
    std::string NewIA = ".lto_discard";
    if (!NonPrevailingAsmSymbols.empty()) {
      // Don't dicard a symbol if there is a live .symver for it.
      ModuleSymbolTable::CollectAsmSymvers(
          M, [&](StringRef Name, StringRef Alias) {
            if (!NonPrevailingAsmSymbols.count(Alias))
              NonPrevailingAsmSymbols.erase(Name);
          });
      NewIA += " " + llvm::join(NonPrevailingAsmSymbols, ", ");
    }
    NewIA += "\n";
    M.setModuleInlineAsm(NewIA + M.getModuleInlineAsm());
  }

  assert(MsymI == MsymE);
  return std::make_pair(std::move(Mod), Res);
}

Error LTO::linkRegularLTO(RegularLTOState::AddedModule Mod,
                          bool LivenessFromIndex) {
  std::vector<GlobalValue *> Keep;
  for (GlobalValue *GV : Mod.Keep) {
    if (LivenessFromIndex && !ThinLTO.CombinedIndex.isGUIDLive(GV->getGUID())) {
      if (Function *F = dyn_cast<Function>(GV)) {
        if (DiagnosticOutputFile) {
          if (Error Err = F->materialize())
            return Err;
          OptimizationRemarkEmitter ORE(F, nullptr);
          ORE.emit(OptimizationRemark(DEBUG_TYPE, "deadfunction", F)
                   << ore::NV("Function", F)
                   << " not added to the combined module ");
        }
      }
      continue;
    }

    if (!GV->hasAvailableExternallyLinkage()) {
      Keep.push_back(GV);
      continue;
    }

    // Only link available_externally definitions if we don't already have a
    // definition.
    GlobalValue *CombinedGV =
        RegularLTO.CombinedModule->getNamedValue(GV->getName());
    if (CombinedGV && !CombinedGV->isDeclaration())
      continue;

    Keep.push_back(GV);
  }

  return RegularLTO.Mover->move(std::move(Mod.M), Keep, nullptr,
                                /* IsPerformingImport */ false);
}

// Add a ThinLTO module to the link.
Expected<ArrayRef<SymbolResolution>>
LTO::addThinLTO(BitcodeModule BM, ArrayRef<InputFile::Symbol> Syms,
                ArrayRef<SymbolResolution> Res) {
  ArrayRef<SymbolResolution> ResTmp = Res;
  for (const InputFile::Symbol &Sym : Syms) {
    assert(!ResTmp.empty());
    const SymbolResolution &R = ResTmp.consume_front();

    if (!Sym.getIRName().empty()) {
      auto GUID = GlobalValue::getGUIDAssumingExternalLinkage(
          GlobalValue::getGlobalIdentifier(Sym.getIRName(),
                                           GlobalValue::ExternalLinkage, ""));
      if (R.Prevailing)
        ThinLTO.PrevailingModuleForGUID[GUID] = BM.getModuleIdentifier();
    }
  }

  if (Error Err =
          BM.readSummary(ThinLTO.CombinedIndex, BM.getModuleIdentifier(),
                         [&](GlobalValue::GUID GUID) {
                           return ThinLTO.PrevailingModuleForGUID[GUID] ==
                                  BM.getModuleIdentifier();
                         }))
    return Err;
  LLVM_DEBUG(dbgs() << "Module " << BM.getModuleIdentifier() << "\n");

  for (const InputFile::Symbol &Sym : Syms) {
    assert(!Res.empty());
    const SymbolResolution &R = Res.consume_front();

    if (!Sym.getIRName().empty()) {
      auto GUID = GlobalValue::getGUIDAssumingExternalLinkage(
          GlobalValue::getGlobalIdentifier(Sym.getIRName(),
                                           GlobalValue::ExternalLinkage, ""));
      if (R.Prevailing) {
        assert(ThinLTO.PrevailingModuleForGUID[GUID] ==
               BM.getModuleIdentifier());

        // For linker redefined symbols (via --wrap or --defsym) we want to
        // switch the linkage to `weak` to prevent IPOs from happening.
        // Find the summary in the module for this very GV and record the new
        // linkage so that we can switch it when we import the GV.
        if (R.LinkerRedefined)
          if (auto S = ThinLTO.CombinedIndex.findSummaryInModule(
                  GUID, BM.getModuleIdentifier()))
            S->setLinkage(GlobalValue::WeakAnyLinkage);
      }

      // If the linker resolved the symbol to a local definition then mark it
      // as local in the summary for the module we are adding.
      if (R.FinalDefinitionInLinkageUnit) {
        if (auto S = ThinLTO.CombinedIndex.findSummaryInModule(
                GUID, BM.getModuleIdentifier())) {
          S->setDSOLocal(true);
        }
      }
    }
  }

  if (!ThinLTO.ModuleMap.insert({BM.getModuleIdentifier(), BM}).second)
    return make_error<StringError>(
        "Expected at most one ThinLTO module per bitcode file",
        inconvertibleErrorCode());

  if (!Conf.ThinLTOModulesToCompile.empty()) {
    if (!ThinLTO.ModulesToCompile)
      ThinLTO.ModulesToCompile = ModuleMapType();
    // This is a fuzzy name matching where only modules with name containing the
    // specified switch values are going to be compiled.
    for (const std::string &Name : Conf.ThinLTOModulesToCompile) {
      if (BM.getModuleIdentifier().contains(Name)) {
        ThinLTO.ModulesToCompile->insert({BM.getModuleIdentifier(), BM});
        LLVM_DEBUG(dbgs() << "[ThinLTO] Selecting " << BM.getModuleIdentifier()
                          << " to compile\n");
      }
    }
  }

  return Res;
}

unsigned LTO::getMaxTasks() const {
  CalledGetMaxTasks = true;
  auto ModuleCount = ThinLTO.ModulesToCompile ? ThinLTO.ModulesToCompile->size()
                                              : ThinLTO.ModuleMap.size();
  return RegularLTO.ParallelCodeGenParallelismLevel + ModuleCount;
}

// If only some of the modules were split, we cannot correctly handle
// code that contains type tests or type checked loads.
Error LTO::checkPartiallySplit() {
  if (!ThinLTO.CombinedIndex.partiallySplitLTOUnits())
    return Error::success();

  const Module *Combined = RegularLTO.CombinedModule.get();
  Function *TypeTestFunc =
      Intrinsic::getDeclarationIfExists(Combined, Intrinsic::type_test);
  Function *TypeCheckedLoadFunc =
      Intrinsic::getDeclarationIfExists(Combined, Intrinsic::type_checked_load);
  Function *TypeCheckedLoadRelativeFunc = Intrinsic::getDeclarationIfExists(
      Combined, Intrinsic::type_checked_load_relative);

  // First check if there are type tests / type checked loads in the
  // merged regular LTO module IR.
  if ((TypeTestFunc && !TypeTestFunc->use_empty()) ||
      (TypeCheckedLoadFunc && !TypeCheckedLoadFunc->use_empty()) ||
      (TypeCheckedLoadRelativeFunc &&
       !TypeCheckedLoadRelativeFunc->use_empty()))
    return make_error<StringError>(
        "inconsistent LTO Unit splitting (recompile with -fsplit-lto-unit)",
        inconvertibleErrorCode());

  // Otherwise check if there are any recorded in the combined summary from the
  // ThinLTO modules.
  for (auto &P : ThinLTO.CombinedIndex) {
    for (auto &S : P.second.SummaryList) {
      auto *FS = dyn_cast<FunctionSummary>(S.get());
      if (!FS)
        continue;
      if (!FS->type_test_assume_vcalls().empty() ||
          !FS->type_checked_load_vcalls().empty() ||
          !FS->type_test_assume_const_vcalls().empty() ||
          !FS->type_checked_load_const_vcalls().empty() ||
          !FS->type_tests().empty())
        return make_error<StringError>(
            "inconsistent LTO Unit splitting (recompile with -fsplit-lto-unit)",
            inconvertibleErrorCode());
    }
  }
  return Error::success();
}

Error LTO::run(AddStreamFn AddStream, FileCache Cache) {
  // Compute "dead" symbols, we don't want to import/export these!
  DenseSet<GlobalValue::GUID> GUIDPreservedSymbols;
  DenseMap<GlobalValue::GUID, PrevailingType> GUIDPrevailingResolutions;
  for (auto &Res : *GlobalResolutions) {
    // Normally resolution have IR name of symbol. We can do nothing here
    // otherwise. See comments in GlobalResolution struct for more details.
    if (Res.second.IRName.empty())
      continue;

    GlobalValue::GUID GUID = GlobalValue::getGUIDAssumingExternalLinkage(
        GlobalValue::dropLLVMManglingEscape(Res.second.IRName));

    if (Res.second.VisibleOutsideSummary && Res.second.Prevailing)
      GUIDPreservedSymbols.insert(GUID);

    if (Res.second.ExportDynamic)
      DynamicExportSymbols.insert(GUID);

    GUIDPrevailingResolutions[GUID] =
        Res.second.Prevailing ? PrevailingType::Yes : PrevailingType::No;
  }

  auto isPrevailing = [&](GlobalValue::GUID G) {
    auto It = GUIDPrevailingResolutions.find(G);
    if (It == GUIDPrevailingResolutions.end())
      return PrevailingType::Unknown;
    return It->second;
  };
  computeDeadSymbolsWithConstProp(ThinLTO.CombinedIndex, GUIDPreservedSymbols,
                                  isPrevailing, Conf.OptLevel > 0);

  // Setup output file to emit statistics.
  auto StatsFileOrErr = setupStatsFile(Conf.StatsFile);
  if (!StatsFileOrErr)
    return StatsFileOrErr.takeError();
  std::unique_ptr<ToolOutputFile> StatsFile = std::move(StatsFileOrErr.get());

  // TODO: Ideally this would be controlled automatically by detecting that we
  // are linking with an allocator that supports these interfaces, rather than
  // an internal option (which would still be needed for tests, however). For
  // example, if the library exported a symbol like __malloc_hot_cold the linker
  // could recognize that and set a flag in the lto::Config.
  if (SupportsHotColdNew)
    ThinLTO.CombinedIndex.setWithSupportsHotColdNew();

  Error Result = runRegularLTO(AddStream);
  if (!Result)
    // This will reset the GlobalResolutions optional once done with it to
    // reduce peak memory before importing.
    Result = runThinLTO(AddStream, Cache, GUIDPreservedSymbols);

  if (StatsFile)
    PrintStatisticsJSON(StatsFile->os());

  return Result;
}

void lto::updateMemProfAttributes(Module &Mod,
                                  const ModuleSummaryIndex &Index) {
  if (Index.withSupportsHotColdNew())
    return;

  // The profile matcher applies hotness attributes directly for allocations,
  // and those will cause us to generate calls to the hot/cold interfaces
  // unconditionally. If supports-hot-cold-new was not enabled in the LTO
  // link then assume we don't want these calls (e.g. not linking with
  // the appropriate library, or otherwise trying to disable this behavior).
  for (auto &F : Mod) {
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *CI = dyn_cast<CallBase>(&I);
        if (!CI)
          continue;
        if (CI->hasFnAttr("memprof"))
          CI->removeFnAttr("memprof");
        // Strip off all memprof metadata as it is no longer needed.
        // Importantly, this avoids the addition of new memprof attributes
        // after inlining propagation.
        // TODO: If we support additional types of MemProf metadata beyond hot
        // and cold, we will need to update the metadata based on the allocator
        // APIs supported instead of completely stripping all.
        CI->setMetadata(LLVMContext::MD_memprof, nullptr);
        CI->setMetadata(LLVMContext::MD_callsite, nullptr);
      }
    }
  }
}

Error LTO::runRegularLTO(AddStreamFn AddStream) {
  // Setup optimization remarks.
  auto DiagFileOrErr = lto::setupLLVMOptimizationRemarks(
      RegularLTO.CombinedModule->getContext(), Conf.RemarksFilename,
      Conf.RemarksPasses, Conf.RemarksFormat, Conf.RemarksWithHotness,
      Conf.RemarksHotnessThreshold);
  LLVM_DEBUG(dbgs() << "Running regular LTO\n");
  if (!DiagFileOrErr)
    return DiagFileOrErr.takeError();
  DiagnosticOutputFile = std::move(*DiagFileOrErr);

  // Finalize linking of regular LTO modules containing summaries now that
  // we have computed liveness information.
  for (auto &M : RegularLTO.ModsWithSummaries)
    if (Error Err = linkRegularLTO(std::move(M),
                                   /*LivenessFromIndex=*/true))
      return Err;

  // Ensure we don't have inconsistently split LTO units with type tests.
  // FIXME: this checks both LTO and ThinLTO. It happens to work as we take
  // this path both cases but eventually this should be split into two and
  // do the ThinLTO checks in `runThinLTO`.
  if (Error Err = checkPartiallySplit())
    return Err;

  // Make sure commons have the right size/alignment: we kept the largest from
  // all the prevailing when adding the inputs, and we apply it here.
  const DataLayout &DL = RegularLTO.CombinedModule->getDataLayout();
  for (auto &I : RegularLTO.Commons) {
    if (!I.second.Prevailing)
      // Don't do anything if no instance of this common was prevailing.
      continue;
    GlobalVariable *OldGV = RegularLTO.CombinedModule->getNamedGlobal(I.first);
    if (OldGV && DL.getTypeAllocSize(OldGV->getValueType()) == I.second.Size) {
      // Don't create a new global if the type is already correct, just make
      // sure the alignment is correct.
      OldGV->setAlignment(I.second.Alignment);
      continue;
    }
    ArrayType *Ty =
        ArrayType::get(Type::getInt8Ty(RegularLTO.Ctx), I.second.Size);
    auto *GV = new GlobalVariable(*RegularLTO.CombinedModule, Ty, false,
                                  GlobalValue::CommonLinkage,
                                  ConstantAggregateZero::get(Ty), "");
    GV->setAlignment(I.second.Alignment);
    if (OldGV) {
      OldGV->replaceAllUsesWith(GV);
      GV->takeName(OldGV);
      OldGV->eraseFromParent();
    } else {
      GV->setName(I.first);
    }
  }

  updateMemProfAttributes(*RegularLTO.CombinedModule, ThinLTO.CombinedIndex);

  bool WholeProgramVisibilityEnabledInLTO =
      Conf.HasWholeProgramVisibility &&
      // If validation is enabled, upgrade visibility only when all vtables
      // have typeinfos.
      (!Conf.ValidateAllVtablesHaveTypeInfos || Conf.AllVtablesHaveTypeInfos);

  // This returns true when the name is local or not defined. Locals are
  // expected to be handled separately.
  auto IsVisibleToRegularObj = [&](StringRef name) {
    auto It = GlobalResolutions->find(name);
    return (It == GlobalResolutions->end() ||
            It->second.VisibleOutsideSummary || !It->second.Prevailing);
  };

  // If allowed, upgrade public vcall visibility metadata to linkage unit
  // visibility before whole program devirtualization in the optimizer.
  updateVCallVisibilityInModule(
      *RegularLTO.CombinedModule, WholeProgramVisibilityEnabledInLTO,
      DynamicExportSymbols, Conf.ValidateAllVtablesHaveTypeInfos,
      IsVisibleToRegularObj);
  updatePublicTypeTestCalls(*RegularLTO.CombinedModule,
                            WholeProgramVisibilityEnabledInLTO);

  if (Conf.PreOptModuleHook &&
      !Conf.PreOptModuleHook(0, *RegularLTO.CombinedModule))
    return finalizeOptimizationRemarks(std::move(DiagnosticOutputFile));

  if (!Conf.CodeGenOnly) {
    for (const auto &R : *GlobalResolutions) {
      GlobalValue *GV =
          RegularLTO.CombinedModule->getNamedValue(R.second.IRName);
      if (!R.second.isPrevailingIRSymbol())
        continue;
      if (R.second.Partition != 0 &&
          R.second.Partition != GlobalResolution::External)
        continue;

      // Ignore symbols defined in other partitions.
      // Also skip declarations, which are not allowed to have internal linkage.
      if (!GV || GV->hasLocalLinkage() || GV->isDeclaration())
        continue;

      // Symbols that are marked DLLImport or DLLExport should not be
      // internalized, as they are either externally visible or referencing
      // external symbols. Symbols that have AvailableExternally or Appending
      // linkage might be used by future passes and should be kept as is.
      // These linkages are seen in Unified regular LTO, because the process
      // of creating split LTO units introduces symbols with that linkage into
      // one of the created modules. Normally, only the ThinLTO backend would
      // compile this module, but Unified Regular LTO processes both
      // modules created by the splitting process as regular LTO modules.
      if ((LTOMode == LTOKind::LTOK_UnifiedRegular) &&
          ((GV->getDLLStorageClass() != GlobalValue::DefaultStorageClass) ||
           GV->hasAvailableExternallyLinkage() || GV->hasAppendingLinkage()))
        continue;

      GV->setUnnamedAddr(R.second.UnnamedAddr ? GlobalValue::UnnamedAddr::Global
                                              : GlobalValue::UnnamedAddr::None);
      if (EnableLTOInternalization && R.second.Partition == 0)
        GV->setLinkage(GlobalValue::InternalLinkage);
    }

    if (Conf.PostInternalizeModuleHook &&
        !Conf.PostInternalizeModuleHook(0, *RegularLTO.CombinedModule))
      return finalizeOptimizationRemarks(std::move(DiagnosticOutputFile));
  }

  if (!RegularLTO.EmptyCombinedModule || Conf.AlwaysEmitRegularLTOObj) {
    if (Error Err =
            backend(Conf, AddStream, RegularLTO.ParallelCodeGenParallelismLevel,
                    *RegularLTO.CombinedModule, ThinLTO.CombinedIndex))
      return Err;
  }

  return finalizeOptimizationRemarks(std::move(DiagnosticOutputFile));
}

SmallVector<const char *> LTO::getRuntimeLibcallSymbols(const Triple &TT) {
  RTLIB::RuntimeLibcallsInfo Libcalls(TT);
  SmallVector<const char *> LibcallSymbols;
  ArrayRef<RTLIB::LibcallImpl> LibcallImpls = Libcalls.getLibcallImpls();
  LibcallSymbols.reserve(LibcallImpls.size());

  for (RTLIB::LibcallImpl Impl : LibcallImpls) {
    if (Impl != RTLIB::Unsupported)
      LibcallSymbols.push_back(Libcalls.getLibcallImplName(Impl));
  }

  return LibcallSymbols;
}

Error ThinBackendProc::emitFiles(
    const FunctionImporter::ImportMapTy &ImportList, llvm::StringRef ModulePath,
    const std::string &NewModulePath) const {
  return emitFiles(ImportList, ModulePath, NewModulePath,
                   NewModulePath + ".thinlto.bc",
                   /*ImportsFiles=*/std::nullopt);
}

Error ThinBackendProc::emitFiles(
    const FunctionImporter::ImportMapTy &ImportList, llvm::StringRef ModulePath,
    const std::string &NewModulePath, StringRef SummaryPath,
    std::optional<std::reference_wrapper<ImportsFilesContainer>> ImportsFiles)
    const {
  ModuleToSummariesForIndexTy ModuleToSummariesForIndex;
  GVSummaryPtrSet DeclarationSummaries;

  std::error_code EC;
  gatherImportedSummariesForModule(ModulePath, ModuleToDefinedGVSummaries,
                                   ImportList, ModuleToSummariesForIndex,
                                   DeclarationSummaries);

  raw_fd_ostream OS(SummaryPath, EC, sys::fs::OpenFlags::OF_None);
  if (EC)
    return createFileError("cannot open " + Twine(SummaryPath), EC);

  writeIndexToFile(CombinedIndex, OS, &ModuleToSummariesForIndex,
                   &DeclarationSummaries);

  if (ShouldEmitImportsFiles) {
    Error ImportsFilesError = EmitImportsFiles(
        ModulePath, NewModulePath + ".imports", ModuleToSummariesForIndex);
    if (ImportsFilesError)
      return ImportsFilesError;
  }

  // Optionally, store the imports files.
  if (ImportsFiles)
    processImportsFiles(
        ModulePath, ModuleToSummariesForIndex,
        [&](StringRef M) { ImportsFiles->get().push_back(M.str()); });

  return Error::success();
}

namespace {
/// Base class for ThinLTO backends that perform code generation and insert the
/// generated files back into the link.
class CGThinBackend : public ThinBackendProc {
protected:
  AddStreamFn AddStream;
  DenseSet<GlobalValue::GUID> CfiFunctionDefs;
  DenseSet<GlobalValue::GUID> CfiFunctionDecls;
  bool ShouldEmitIndexFiles;

public:
  CGThinBackend(
      const Config &Conf, ModuleSummaryIndex &CombinedIndex,
      const DenseMap<StringRef, GVSummaryMapTy> &ModuleToDefinedGVSummaries,
      AddStreamFn AddStream, lto::IndexWriteCallback OnWrite,
      bool ShouldEmitIndexFiles, bool ShouldEmitImportsFiles,
      ThreadPoolStrategy ThinLTOParallelism)
      : ThinBackendProc(Conf, CombinedIndex, ModuleToDefinedGVSummaries,
                        OnWrite, ShouldEmitImportsFiles, ThinLTOParallelism),
        AddStream(std::move(AddStream)),
        ShouldEmitIndexFiles(ShouldEmitIndexFiles) {
    auto &Defs = CombinedIndex.cfiFunctionDefs();
    CfiFunctionDefs.insert_range(Defs.guids());
    auto &Decls = CombinedIndex.cfiFunctionDecls();
    CfiFunctionDecls.insert_range(Decls.guids());
  }
};

/// This backend performs code generation by scheduling a job to run on
/// an in-process thread when invoked for each task.
class InProcessThinBackend : public CGThinBackend {
protected:
  FileCache Cache;

public:
  InProcessThinBackend(
      const Config &Conf, ModuleSummaryIndex &CombinedIndex,
      ThreadPoolStrategy ThinLTOParallelism,
      const DenseMap<StringRef, GVSummaryMapTy> &ModuleToDefinedGVSummaries,
      AddStreamFn AddStream, FileCache Cache, lto::IndexWriteCallback OnWrite,
      bool ShouldEmitIndexFiles, bool ShouldEmitImportsFiles)
      : CGThinBackend(Conf, CombinedIndex, ModuleToDefinedGVSummaries,
                      AddStream, OnWrite, ShouldEmitIndexFiles,
                      ShouldEmitImportsFiles, ThinLTOParallelism),
        Cache(std::move(Cache)) {}

  virtual Error runThinLTOBackendThread(
      AddStreamFn AddStream, FileCache Cache, unsigned Task, BitcodeModule BM,
      ModuleSummaryIndex &CombinedIndex,
      const FunctionImporter::ImportMapTy &ImportList,
      const FunctionImporter::ExportSetTy &ExportList,
      const std::map<GlobalValue::GUID, GlobalValue::LinkageTypes> &ResolvedODR,
      const GVSummaryMapTy &DefinedGlobals,
      MapVector<StringRef, BitcodeModule> &ModuleMap) {
    auto RunThinBackend = [&](AddStreamFn AddStream) {
      LTOLLVMContext BackendContext(Conf);
      Expected<std::unique_ptr<Module>> MOrErr = BM.parseModule(BackendContext);
      if (!MOrErr)
        return MOrErr.takeError();

      return thinBackend(Conf, Task, AddStream, **MOrErr, CombinedIndex,
                         ImportList, DefinedGlobals, &ModuleMap,
                         Conf.CodeGenOnly);
    };

    auto ModuleID = BM.getModuleIdentifier();

    if (ShouldEmitIndexFiles) {
      if (auto E = emitFiles(ImportList, ModuleID, ModuleID.str()))
        return E;
    }

    if (!Cache.isValid() || !CombinedIndex.modulePaths().count(ModuleID) ||
        all_of(CombinedIndex.getModuleHash(ModuleID),
               [](uint32_t V) { return V == 0; }))
      // Cache disabled or no entry for this module in the combined index or
      // no module hash.
      return RunThinBackend(AddStream);

    // The module may be cached, this helps handling it.
    std::string Key = computeLTOCacheKey(
        Conf, CombinedIndex, ModuleID, ImportList, ExportList, ResolvedODR,
        DefinedGlobals, CfiFunctionDefs, CfiFunctionDecls);
    Expected<AddStreamFn> CacheAddStreamOrErr = Cache(Task, Key, ModuleID);
    if (Error Err = CacheAddStreamOrErr.takeError())
      return Err;
    AddStreamFn &CacheAddStream = *CacheAddStreamOrErr;
    if (CacheAddStream)
      return RunThinBackend(CacheAddStream);

    return Error::success();
  }

  Error start(
      unsigned Task, BitcodeModule BM,
      const FunctionImporter::ImportMapTy &ImportList,
      const FunctionImporter::ExportSetTy &ExportList,
      const std::map<GlobalValue::GUID, GlobalValue::LinkageTypes> &ResolvedODR,
      MapVector<StringRef, BitcodeModule> &ModuleMap) override {
    StringRef ModulePath = BM.getModuleIdentifier();
    assert(ModuleToDefinedGVSummaries.count(ModulePath));
    const GVSummaryMapTy &DefinedGlobals =
        ModuleToDefinedGVSummaries.find(ModulePath)->second;
    BackendThreadPool.async(
        [=](BitcodeModule BM, ModuleSummaryIndex &CombinedIndex,
            const FunctionImporter::ImportMapTy &ImportList,
            const FunctionImporter::ExportSetTy &ExportList,
            const std::map<GlobalValue::GUID, GlobalValue::LinkageTypes>
                &ResolvedODR,
            const GVSummaryMapTy &DefinedGlobals,
            MapVector<StringRef, BitcodeModule> &ModuleMap) {
          if (LLVM_ENABLE_THREADS && Conf.TimeTraceEnabled)
            timeTraceProfilerInitialize(Conf.TimeTraceGranularity,
                                        "thin backend");
          Error E = runThinLTOBackendThread(
              AddStream, Cache, Task, BM, CombinedIndex, ImportList, ExportList,
              ResolvedODR, DefinedGlobals, ModuleMap);
          if (E) {
            std::unique_lock<std::mutex> L(ErrMu);
            if (Err)
              Err = joinErrors(std::move(*Err), std::move(E));
            else
              Err = std::move(E);
          }
          if (LLVM_ENABLE_THREADS && Conf.TimeTraceEnabled)
            timeTraceProfilerFinishThread();
        },
        BM, std::ref(CombinedIndex), std::ref(ImportList), std::ref(ExportList),
        std::ref(ResolvedODR), std::ref(DefinedGlobals), std::ref(ModuleMap));

    if (OnWrite)
      OnWrite(std::string(ModulePath));
    return Error::success();
  }
};

/// This backend is utilized in the first round of a two-codegen round process.
/// It first saves optimized bitcode files to disk before the codegen process
/// begins. After codegen, it stores the resulting object files in a scratch
/// buffer. Note the codegen data stored in the scratch buffer will be extracted
/// and merged in the subsequent step.
class FirstRoundThinBackend : public InProcessThinBackend {
  AddStreamFn IRAddStream;
  FileCache IRCache;

public:
  FirstRoundThinBackend(
      const Config &Conf, ModuleSummaryIndex &CombinedIndex,
      ThreadPoolStrategy ThinLTOParallelism,
      const DenseMap<StringRef, GVSummaryMapTy> &ModuleToDefinedGVSummaries,
      AddStreamFn CGAddStream, FileCache CGCache, AddStreamFn IRAddStream,
      FileCache IRCache)
      : InProcessThinBackend(Conf, CombinedIndex, ThinLTOParallelism,
                             ModuleToDefinedGVSummaries, std::move(CGAddStream),
                             std::move(CGCache), /*OnWrite=*/nullptr,
                             /*ShouldEmitIndexFiles=*/false,
                             /*ShouldEmitImportsFiles=*/false),
        IRAddStream(std::move(IRAddStream)), IRCache(std::move(IRCache)) {}

  Error runThinLTOBackendThread(
      AddStreamFn CGAddStream, FileCache CGCache, unsigned Task,
      BitcodeModule BM, ModuleSummaryIndex &CombinedIndex,
      const FunctionImporter::ImportMapTy &ImportList,
      const FunctionImporter::ExportSetTy &ExportList,
      const std::map<GlobalValue::GUID, GlobalValue::LinkageTypes> &ResolvedODR,
      const GVSummaryMapTy &DefinedGlobals,
      MapVector<StringRef, BitcodeModule> &ModuleMap) override {
    auto RunThinBackend = [&](AddStreamFn CGAddStream,
                              AddStreamFn IRAddStream) {
      LTOLLVMContext BackendContext(Conf);
      Expected<std::unique_ptr<Module>> MOrErr = BM.parseModule(BackendContext);
      if (!MOrErr)
        return MOrErr.takeError();

      return thinBackend(Conf, Task, CGAddStream, **MOrErr, CombinedIndex,
                         ImportList, DefinedGlobals, &ModuleMap,
                         Conf.CodeGenOnly, IRAddStream);
    };

    auto ModuleID = BM.getModuleIdentifier();
    // Like InProcessThinBackend, we produce index files as needed for
    // FirstRoundThinBackend. However, these files are not generated for
    // SecondRoundThinBackend.
    if (ShouldEmitIndexFiles) {
      if (auto E = emitFiles(ImportList, ModuleID, ModuleID.str()))
        return E;
    }

    assert((CGCache.isValid() == IRCache.isValid()) &&
           "Both caches for CG and IR should have matching availability");
    if (!CGCache.isValid() || !CombinedIndex.modulePaths().count(ModuleID) ||
        all_of(CombinedIndex.getModuleHash(ModuleID),
               [](uint32_t V) { return V == 0; }))
      // Cache disabled or no entry for this module in the combined index or
      // no module hash.
      return RunThinBackend(CGAddStream, IRAddStream);

    // Get CGKey for caching object in CGCache.
    std::string CGKey = computeLTOCacheKey(
        Conf, CombinedIndex, ModuleID, ImportList, ExportList, ResolvedODR,
        DefinedGlobals, CfiFunctionDefs, CfiFunctionDecls);
    Expected<AddStreamFn> CacheCGAddStreamOrErr =
        CGCache(Task, CGKey, ModuleID);
    if (Error Err = CacheCGAddStreamOrErr.takeError())
      return Err;
    AddStreamFn &CacheCGAddStream = *CacheCGAddStreamOrErr;

    // Get IRKey for caching (optimized) IR in IRCache with an extra ID.
    std::string IRKey = recomputeLTOCacheKey(CGKey, /*ExtraID=*/"IR");
    Expected<AddStreamFn> CacheIRAddStreamOrErr =
        IRCache(Task, IRKey, ModuleID);
    if (Error Err = CacheIRAddStreamOrErr.takeError())
      return Err;
    AddStreamFn &CacheIRAddStream = *CacheIRAddStreamOrErr;

    // Ideally, both CG and IR caching should be synchronized. However, in
    // practice, their availability may differ due to different expiration
    // times. Therefore, if either cache is missing, the backend process is
    // triggered.
    if (CacheCGAddStream || CacheIRAddStream) {
      LLVM_DEBUG(dbgs() << "[FirstRound] Cache Miss for "
                        << BM.getModuleIdentifier() << "\n");
      return RunThinBackend(CacheCGAddStream ? CacheCGAddStream : CGAddStream,
                            CacheIRAddStream ? CacheIRAddStream : IRAddStream);
    }

    return Error::success();
  }
};

/// This backend operates in the second round of a two-codegen round process.
/// It starts by reading the optimized bitcode files that were saved during the
/// first round. The backend then executes the codegen only to further optimize
/// the code, utilizing the codegen data merged from the first round. Finally,
/// it writes the resulting object files as usual.
class SecondRoundThinBackend : public InProcessThinBackend {
  std::unique_ptr<SmallVector<StringRef>> IRFiles;
  stable_hash CombinedCGDataHash;

public:
  SecondRoundThinBackend(
      const Config &Conf, ModuleSummaryIndex &CombinedIndex,
      ThreadPoolStrategy ThinLTOParallelism,
      const DenseMap<StringRef, GVSummaryMapTy> &ModuleToDefinedGVSummaries,
      AddStreamFn AddStream, FileCache Cache,
      std::unique_ptr<SmallVector<StringRef>> IRFiles,
      stable_hash CombinedCGDataHash)
      : InProcessThinBackend(Conf, CombinedIndex, ThinLTOParallelism,
                             ModuleToDefinedGVSummaries, std::move(AddStream),
                             std::move(Cache),
                             /*OnWrite=*/nullptr,
                             /*ShouldEmitIndexFiles=*/false,
                             /*ShouldEmitImportsFiles=*/false),
        IRFiles(std::move(IRFiles)), CombinedCGDataHash(CombinedCGDataHash) {}

  virtual Error runThinLTOBackendThread(
      AddStreamFn AddStream, FileCache Cache, unsigned Task, BitcodeModule BM,
      ModuleSummaryIndex &CombinedIndex,
      const FunctionImporter::ImportMapTy &ImportList,
      const FunctionImporter::ExportSetTy &ExportList,
      const std::map<GlobalValue::GUID, GlobalValue::LinkageTypes> &ResolvedODR,
      const GVSummaryMapTy &DefinedGlobals,
      MapVector<StringRef, BitcodeModule> &ModuleMap) override {
    auto RunThinBackend = [&](AddStreamFn AddStream) {
      LTOLLVMContext BackendContext(Conf);
      std::unique_ptr<Module> LoadedModule =
          cgdata::loadModuleForTwoRounds(BM, Task, BackendContext, *IRFiles);

      return thinBackend(Conf, Task, AddStream, *LoadedModule, CombinedIndex,
                         ImportList, DefinedGlobals, &ModuleMap,
                         /*CodeGenOnly=*/true);
    };

    auto ModuleID = BM.getModuleIdentifier();
    if (!Cache.isValid() || !CombinedIndex.modulePaths().count(ModuleID) ||
        all_of(CombinedIndex.getModuleHash(ModuleID),
               [](uint32_t V) { return V == 0; }))
      // Cache disabled or no entry for this module in the combined index or
      // no module hash.
      return RunThinBackend(AddStream);

    // Get Key for caching the final object file in Cache with the combined
    // CGData hash.
    std::string Key = computeLTOCacheKey(
        Conf, CombinedIndex, ModuleID, ImportList, ExportList, ResolvedODR,
        DefinedGlobals, CfiFunctionDefs, CfiFunctionDecls);
    Key = recomputeLTOCacheKey(Key,
                               /*ExtraID=*/std::to_string(CombinedCGDataHash));
    Expected<AddStreamFn> CacheAddStreamOrErr = Cache(Task, Key, ModuleID);
    if (Error Err = CacheAddStreamOrErr.takeError())
      return Err;
    AddStreamFn &CacheAddStream = *CacheAddStreamOrErr;

    if (CacheAddStream) {
      LLVM_DEBUG(dbgs() << "[SecondRound] Cache Miss for "
                        << BM.getModuleIdentifier() << "\n");
      return RunThinBackend(CacheAddStream);
    }

    return Error::success();
  }
};
} // end anonymous namespace

ThinBackend lto::createInProcessThinBackend(ThreadPoolStrategy Parallelism,
                                            lto::IndexWriteCallback OnWrite,
                                            bool ShouldEmitIndexFiles,
                                            bool ShouldEmitImportsFiles) {
  auto Func =
      [=](const Config &Conf, ModuleSummaryIndex &CombinedIndex,
          const DenseMap<StringRef, GVSummaryMapTy> &ModuleToDefinedGVSummaries,
          AddStreamFn AddStream, FileCache Cache) {
        return std::make_unique<InProcessThinBackend>(
            Conf, CombinedIndex, Parallelism, ModuleToDefinedGVSummaries,
            AddStream, Cache, OnWrite, ShouldEmitIndexFiles,
            ShouldEmitImportsFiles);
      };
  return ThinBackend(Func, Parallelism);
}

StringLiteral lto::getThinLTODefaultCPU(const Triple &TheTriple) {
  if (!TheTriple.isOSDarwin())
    return "";
  if (TheTriple.getArch() == Triple::x86_64)
    return "core2";
  if (TheTriple.getArch() == Triple::x86)
    return "yonah";
  if (TheTriple.isArm64e())
    return "apple-a12";
  if (TheTriple.getArch() == Triple::aarch64 ||
      TheTriple.getArch() == Triple::aarch64_32)
    return "cyclone";
  return "";
}

// Given the original \p Path to an output file, replace any path
// prefix matching \p OldPrefix with \p NewPrefix. Also, create the
// resulting directory if it does not yet exist.
std::string lto::getThinLTOOutputFile(StringRef Path, StringRef OldPrefix,
                                      StringRef NewPrefix) {
  if (OldPrefix.empty() && NewPrefix.empty())
    return std::string(Path);
  SmallString<128> NewPath(Path);
  llvm::sys::path::replace_path_prefix(NewPath, OldPrefix, NewPrefix);
  StringRef ParentPath = llvm::sys::path::parent_path(NewPath.str());
  if (!ParentPath.empty()) {
    // Make sure the new directory exists, creating it if necessary.
    if (std::error_code EC = llvm::sys::fs::create_directories(ParentPath))
      llvm::errs() << "warning: could not create directory '" << ParentPath
                   << "': " << EC.message() << '\n';
  }
  return std::string(NewPath);
}

namespace {
class WriteIndexesThinBackend : public ThinBackendProc {
  std::string OldPrefix, NewPrefix, NativeObjectPrefix;
  raw_fd_ostream *LinkedObjectsFile;

public:
  WriteIndexesThinBackend(
      const Config &Conf, ModuleSummaryIndex &CombinedIndex,
      ThreadPoolStrategy ThinLTOParallelism,
      const DenseMap<StringRef, GVSummaryMapTy> &ModuleToDefinedGVSummaries,
      std::string OldPrefix, std::string NewPrefix,
      std::string NativeObjectPrefix, bool ShouldEmitImportsFiles,
      raw_fd_ostream *LinkedObjectsFile, lto::IndexWriteCallback OnWrite)
      : ThinBackendProc(Conf, CombinedIndex, ModuleToDefinedGVSummaries,
                        OnWrite, ShouldEmitImportsFiles, ThinLTOParallelism),
        OldPrefix(OldPrefix), NewPrefix(NewPrefix),
        NativeObjectPrefix(NativeObjectPrefix),
        LinkedObjectsFile(LinkedObjectsFile) {}

  Error start(
      unsigned Task, BitcodeModule BM,
      const FunctionImporter::ImportMapTy &ImportList,
      const FunctionImporter::ExportSetTy &ExportList,
      const std::map<GlobalValue::GUID, GlobalValue::LinkageTypes> &ResolvedODR,
      MapVector<StringRef, BitcodeModule> &ModuleMap) override {
    StringRef ModulePath = BM.getModuleIdentifier();

    // The contents of this file may be used as input to a native link, and must
    // therefore contain the processed modules in a determinstic order that
    // match the order they are provided on the command line. For that reason,
    // we cannot include this in the asynchronously executed lambda below.
    if (LinkedObjectsFile) {
      std::string ObjectPrefix =
          NativeObjectPrefix.empty() ? NewPrefix : NativeObjectPrefix;
      std::string LinkedObjectsFilePath =
          getThinLTOOutputFile(ModulePath, OldPrefix, ObjectPrefix);
      *LinkedObjectsFile << LinkedObjectsFilePath << '\n';
    }

    BackendThreadPool.async(
        [this](const StringRef ModulePath,
               const FunctionImporter::ImportMapTy &ImportList,
               const std::string &OldPrefix, const std::string &NewPrefix) {
          std::string NewModulePath =
              getThinLTOOutputFile(ModulePath, OldPrefix, NewPrefix);
          auto E = emitFiles(ImportList, ModulePath, NewModulePath);
          if (E) {
            std::unique_lock<std::mutex> L(ErrMu);
            if (Err)
              Err = joinErrors(std::move(*Err), std::move(E));
            else
              Err = std::move(E);
            return;
          }
        },
        ModulePath, ImportList, OldPrefix, NewPrefix);

    if (OnWrite)
      OnWrite(std::string(ModulePath));
    return Error::success();
  }

  bool isSensitiveToInputOrder() override {
    // The order which modules are written to LinkedObjectsFile should be
    // deterministic and match the order they are passed on the command line.
    return true;
  }
};
} // end anonymous namespace

ThinBackend lto::createWriteIndexesThinBackend(
    ThreadPoolStrategy Parallelism, std::string OldPrefix,
    std::string NewPrefix, std::string NativeObjectPrefix,
    bool ShouldEmitImportsFiles, raw_fd_ostream *LinkedObjectsFile,
    IndexWriteCallback OnWrite) {
  auto Func =
      [=](const Config &Conf, ModuleSummaryIndex &CombinedIndex,
          const DenseMap<StringRef, GVSummaryMapTy> &ModuleToDefinedGVSummaries,
          AddStreamFn AddStream, FileCache Cache) {
        return std::make_unique<WriteIndexesThinBackend>(
            Conf, CombinedIndex, Parallelism, ModuleToDefinedGVSummaries,
            OldPrefix, NewPrefix, NativeObjectPrefix, ShouldEmitImportsFiles,
            LinkedObjectsFile, OnWrite);
      };
  return ThinBackend(Func, Parallelism);
}

Error LTO::runThinLTO(AddStreamFn AddStream, FileCache Cache,
                      const DenseSet<GlobalValue::GUID> &GUIDPreservedSymbols) {
  LLVM_DEBUG(dbgs() << "Running ThinLTO\n");
  ThinLTO.CombinedIndex.releaseTemporaryMemory();
  timeTraceProfilerBegin("ThinLink", StringRef(""));
  auto TimeTraceScopeExit = llvm::make_scope_exit([]() {
    if (llvm::timeTraceProfilerEnabled())
      llvm::timeTraceProfilerEnd();
  });
  if (ThinLTO.ModuleMap.empty())
    return Error::success();

  if (ThinLTO.ModulesToCompile && ThinLTO.ModulesToCompile->empty()) {
    llvm::errs() << "warning: [ThinLTO] No module compiled\n";
    return Error::success();
  }

  if (Conf.CombinedIndexHook &&
      !Conf.CombinedIndexHook(ThinLTO.CombinedIndex, GUIDPreservedSymbols))
    return Error::success();

  // Collect for each module the list of function it defines (GUID ->
  // Summary).
  DenseMap<StringRef, GVSummaryMapTy> ModuleToDefinedGVSummaries(
      ThinLTO.ModuleMap.size());
  ThinLTO.CombinedIndex.collectDefinedGVSummariesPerModule(
      ModuleToDefinedGVSummaries);
  // Create entries for any modules that didn't have any GV summaries
  // (either they didn't have any GVs to start with, or we suppressed
  // generation of the summaries because they e.g. had inline assembly
  // uses that couldn't be promoted/renamed on export). This is so
  // InProcessThinBackend::start can still launch a backend thread, which
  // is passed the map of summaries for the module, without any special
  // handling for this case.
  for (auto &Mod : ThinLTO.ModuleMap)
    if (!ModuleToDefinedGVSummaries.count(Mod.first))
      ModuleToDefinedGVSummaries.try_emplace(Mod.first);

  FunctionImporter::ImportListsTy ImportLists(ThinLTO.ModuleMap.size());
  DenseMap<StringRef, FunctionImporter::ExportSetTy> ExportLists(
      ThinLTO.ModuleMap.size());
  StringMap<std::map<GlobalValue::GUID, GlobalValue::LinkageTypes>> ResolvedODR;

  if (DumpThinCGSCCs)
    ThinLTO.CombinedIndex.dumpSCCs(outs());

  std::set<GlobalValue::GUID> ExportedGUIDs;

  bool WholeProgramVisibilityEnabledInLTO =
      Conf.HasWholeProgramVisibility &&
      // If validation is enabled, upgrade visibility only when all vtables
      // have typeinfos.
      (!Conf.ValidateAllVtablesHaveTypeInfos || Conf.AllVtablesHaveTypeInfos);
  if (hasWholeProgramVisibility(WholeProgramVisibilityEnabledInLTO))
    ThinLTO.CombinedIndex.setWithWholeProgramVisibility();

  // If we're validating, get the vtable symbols that should not be
  // upgraded because they correspond to typeIDs outside of index-based
  // WPD info.
  DenseSet<GlobalValue::GUID> VisibleToRegularObjSymbols;
  if (WholeProgramVisibilityEnabledInLTO &&
      Conf.ValidateAllVtablesHaveTypeInfos) {
    // This returns true when the name is local or not defined. Locals are
    // expected to be handled separately.
    auto IsVisibleToRegularObj = [&](StringRef name) {
      auto It = GlobalResolutions->find(name);
      return (It == GlobalResolutions->end() ||
              It->second.VisibleOutsideSummary || !It->second.Prevailing);
    };

    getVisibleToRegularObjVtableGUIDs(ThinLTO.CombinedIndex,
                                      VisibleToRegularObjSymbols,
                                      IsVisibleToRegularObj);
  }

  // If allowed, upgrade public vcall visibility to linkage unit visibility in
  // the summaries before whole program devirtualization below.
  updateVCallVisibilityInIndex(
      ThinLTO.CombinedIndex, WholeProgramVisibilityEnabledInLTO,
      DynamicExportSymbols, VisibleToRegularObjSymbols);

  // Perform index-based WPD. This will return immediately if there are
  // no index entries in the typeIdMetadata map (e.g. if we are instead
  // performing IR-based WPD in hybrid regular/thin LTO mode).
  std::map<ValueInfo, std::vector<VTableSlotSummary>> LocalWPDTargetsMap;
  runWholeProgramDevirtOnIndex(ThinLTO.CombinedIndex, ExportedGUIDs,
                               LocalWPDTargetsMap);

  auto isPrevailing = [&](GlobalValue::GUID GUID, const GlobalValueSummary *S) {
    return ThinLTO.PrevailingModuleForGUID[GUID] == S->modulePath();
  };
  if (EnableMemProfContextDisambiguation) {
    MemProfContextDisambiguation ContextDisambiguation;
    ContextDisambiguation.run(ThinLTO.CombinedIndex, isPrevailing);
  }

  // Figure out which symbols need to be internalized. This also needs to happen
  // at -O0 because summary-based DCE is implemented using internalization, and
  // we must apply DCE consistently with the full LTO module in order to avoid
  // undefined references during the final link.
  for (auto &Res : *GlobalResolutions) {
    // If the symbol does not have external references or it is not prevailing,
    // then not need to mark it as exported from a ThinLTO partition.
    if (Res.second.Partition != GlobalResolution::External ||
        !Res.second.isPrevailingIRSymbol())
      continue;
    auto GUID = GlobalValue::getGUIDAssumingExternalLinkage(
        GlobalValue::dropLLVMManglingEscape(Res.second.IRName));
    // Mark exported unless index-based analysis determined it to be dead.
    if (ThinLTO.CombinedIndex.isGUIDLive(GUID))
      ExportedGUIDs.insert(GUID);
  }

  // Reset the GlobalResolutions to deallocate the associated memory, as there
  // are no further accesses. We specifically want to do this before computing
  // cross module importing, which adds to peak memory via the computed import
  // and export lists.
  releaseGlobalResolutionsMemory();

  if (Conf.OptLevel > 0)
    ComputeCrossModuleImport(ThinLTO.CombinedIndex, ModuleToDefinedGVSummaries,
                             isPrevailing, ImportLists, ExportLists);

  // Any functions referenced by the jump table in the regular LTO object must
  // be exported.
  auto &Defs = ThinLTO.CombinedIndex.cfiFunctionDefs();
  ExportedGUIDs.insert(Defs.guid_begin(), Defs.guid_end());
  auto &Decls = ThinLTO.CombinedIndex.cfiFunctionDecls();
  ExportedGUIDs.insert(Decls.guid_begin(), Decls.guid_end());

  auto isExported = [&](StringRef ModuleIdentifier, ValueInfo VI) {
    const auto &ExportList = ExportLists.find(ModuleIdentifier);
    return (ExportList != ExportLists.end() && ExportList->second.count(VI)) ||
           ExportedGUIDs.count(VI.getGUID());
  };

  // Update local devirtualized targets that were exported by cross-module
  // importing or by other devirtualizations marked in the ExportedGUIDs set.
  updateIndexWPDForExports(ThinLTO.CombinedIndex, isExported,
                           LocalWPDTargetsMap);

  thinLTOInternalizeAndPromoteInIndex(ThinLTO.CombinedIndex, isExported,
                                      isPrevailing);

  auto recordNewLinkage = [&](StringRef ModuleIdentifier,
                              GlobalValue::GUID GUID,
                              GlobalValue::LinkageTypes NewLinkage) {
    ResolvedODR[ModuleIdentifier][GUID] = NewLinkage;
  };
  thinLTOResolvePrevailingInIndex(Conf, ThinLTO.CombinedIndex, isPrevailing,
                                  recordNewLinkage, GUIDPreservedSymbols);

  thinLTOPropagateFunctionAttrs(ThinLTO.CombinedIndex, isPrevailing);

  generateParamAccessSummary(ThinLTO.CombinedIndex);

  if (llvm::timeTraceProfilerEnabled())
    llvm::timeTraceProfilerEnd();

  TimeTraceScopeExit.release();

  auto &ModuleMap =
      ThinLTO.ModulesToCompile ? *ThinLTO.ModulesToCompile : ThinLTO.ModuleMap;

  auto RunBackends = [&](ThinBackendProc *BackendProcess) -> Error {
    auto ProcessOneModule = [&](int I) -> Error {
      auto &Mod = *(ModuleMap.begin() + I);
      // Tasks 0 through ParallelCodeGenParallelismLevel-1 are reserved for
      // combined module and parallel code generation partitions.
      return BackendProcess->start(
          RegularLTO.ParallelCodeGenParallelismLevel + I, Mod.second,
          ImportLists[Mod.first], ExportLists[Mod.first],
          ResolvedODR[Mod.first], ThinLTO.ModuleMap);
    };

    BackendProcess->setup(ModuleMap.size(),
                          RegularLTO.ParallelCodeGenParallelismLevel,
                          RegularLTO.CombinedModule->getTargetTriple());

    if (BackendProcess->getThreadCount() == 1 ||
        BackendProcess->isSensitiveToInputOrder()) {
      // Process the modules in the order they were provided on the
      // command-line. It is important for this codepath to be used for
      // WriteIndexesThinBackend, to ensure the emitted LinkedObjectsFile lists
      // ThinLTO objects in the same order as the inputs, which otherwise would
      // affect the final link order.
      for (int I = 0, E = ModuleMap.size(); I != E; ++I)
        if (Error E = ProcessOneModule(I))
          return E;
    } else {
      // When executing in parallel, process largest bitsize modules first to
      // improve parallelism, and avoid starving the thread pool near the end.
      // This saves about 15 sec on a 36-core machine while link `clang.exe`
      // (out of 100 sec).
      std::vector<BitcodeModule *> ModulesVec;
      ModulesVec.reserve(ModuleMap.size());
      for (auto &Mod : ModuleMap)
        ModulesVec.push_back(&Mod.second);
      for (int I : generateModulesOrdering(ModulesVec))
        if (Error E = ProcessOneModule(I))
          return E;
    }
    return BackendProcess->wait();
  };

  if (!CodeGenDataThinLTOTwoRounds) {
    std::unique_ptr<ThinBackendProc> BackendProc =
        ThinLTO.Backend(Conf, ThinLTO.CombinedIndex, ModuleToDefinedGVSummaries,
                        AddStream, Cache);
    return RunBackends(BackendProc.get());
  }

  // Perform two rounds of code generation for ThinLTO:
  // 1. First round: Perform optimization and code generation, outputting to
  // temporary scratch objects.
  // 2. Merge code generation data extracted from the temporary scratch objects.
  // 3. Second round: Execute code generation again using the merged data.
  LLVM_DEBUG(dbgs() << "[TwoRounds] Initializing ThinLTO two-codegen rounds\n");

  unsigned MaxTasks = getMaxTasks();
  auto Parallelism = ThinLTO.Backend.getParallelism();
  // Set up two additional streams and caches for storing temporary scratch
  // objects and optimized IRs, using the same cache directory as the original.
  cgdata::StreamCacheData CG(MaxTasks, Cache, "CG"), IR(MaxTasks, Cache, "IR");

  // First round: Execute optimization and code generation, outputting to
  // temporary scratch objects. Serialize the optimized IRs before initiating
  // code generation.
  LLVM_DEBUG(dbgs() << "[TwoRounds] Running the first round of codegen\n");
  auto FirstRoundLTO = std::make_unique<FirstRoundThinBackend>(
      Conf, ThinLTO.CombinedIndex, Parallelism, ModuleToDefinedGVSummaries,
      CG.AddStream, CG.Cache, IR.AddStream, IR.Cache);
  if (Error E = RunBackends(FirstRoundLTO.get()))
    return E;

  LLVM_DEBUG(dbgs() << "[TwoRounds] Merging codegen data\n");
  auto CombinedHashOrErr = cgdata::mergeCodeGenData(*CG.getResult());
  if (Error E = CombinedHashOrErr.takeError())
    return E;
  auto CombinedHash = *CombinedHashOrErr;
  LLVM_DEBUG(dbgs() << "[TwoRounds] CGData hash: " << CombinedHash << "\n");

  // Second round: Read the optimized IRs and execute code generation using the
  // merged data.
  LLVM_DEBUG(dbgs() << "[TwoRounds] Running the second round of codegen\n");
  auto SecondRoundLTO = std::make_unique<SecondRoundThinBackend>(
      Conf, ThinLTO.CombinedIndex, Parallelism, ModuleToDefinedGVSummaries,
      AddStream, Cache, IR.getResult(), CombinedHash);
  return RunBackends(SecondRoundLTO.get());
}

Expected<std::unique_ptr<ToolOutputFile>> lto::setupLLVMOptimizationRemarks(
    LLVMContext &Context, StringRef RemarksFilename, StringRef RemarksPasses,
    StringRef RemarksFormat, bool RemarksWithHotness,
    std::optional<uint64_t> RemarksHotnessThreshold, int Count) {
  std::string Filename = std::string(RemarksFilename);
  // For ThinLTO, file.opt.<format> becomes
  // file.opt.<format>.thin.<num>.<format>.
  if (!Filename.empty() && Count != -1)
    Filename =
        (Twine(Filename) + ".thin." + llvm::utostr(Count) + "." + RemarksFormat)
            .str();

  auto ResultOrErr = llvm::setupLLVMOptimizationRemarks(
      Context, Filename, RemarksPasses, RemarksFormat, RemarksWithHotness,
      RemarksHotnessThreshold);
  if (Error E = ResultOrErr.takeError())
    return std::move(E);

  if (*ResultOrErr)
    (*ResultOrErr)->keep();

  return ResultOrErr;
}

Expected<std::unique_ptr<ToolOutputFile>>
lto::setupStatsFile(StringRef StatsFilename) {
  // Setup output file to emit statistics.
  if (StatsFilename.empty())
    return nullptr;

  llvm::EnableStatistics(false);
  std::error_code EC;
  auto StatsFile =
      std::make_unique<ToolOutputFile>(StatsFilename, EC, sys::fs::OF_None);
  if (EC)
    return errorCodeToError(EC);

  StatsFile->keep();
  return std::move(StatsFile);
}

// Compute the ordering we will process the inputs: the rough heuristic here
// is to sort them per size so that the largest module get schedule as soon as
// possible. This is purely a compile-time optimization.
std::vector<int> lto::generateModulesOrdering(ArrayRef<BitcodeModule *> R) {
  auto Seq = llvm::seq<int>(0, R.size());
  std::vector<int> ModulesOrdering(Seq.begin(), Seq.end());
  llvm::sort(ModulesOrdering, [&](int LeftIndex, int RightIndex) {
    auto LSize = R[LeftIndex]->getBuffer().size();
    auto RSize = R[RightIndex]->getBuffer().size();
    return LSize > RSize;
  });
  return ModulesOrdering;
}

namespace {
/// This out-of-process backend does not perform code generation when invoked
/// for each task. Instead, it generates the necessary information (e.g., the
/// summary index shard, import list, etc.) to enable code generation to be
/// performed externally, similar to WriteIndexesThinBackend. The backend's
/// `wait` function then invokes an external distributor process to carry out
/// the backend compilations.
class OutOfProcessThinBackend : public CGThinBackend {
  using SString = SmallString<128>;

  BumpPtrAllocator Alloc;
  StringSaver Saver{Alloc};

  SString LinkerOutputFile;

  SString DistributorPath;
  ArrayRef<StringRef> DistributorArgs;

  SString RemoteCompiler;
  ArrayRef<StringRef> RemoteCompilerArgs;

  bool SaveTemps;

  SmallVector<StringRef, 0> CodegenOptions;
  DenseSet<StringRef> CommonInputs;

  // Information specific to individual backend compilation job.
  struct Job {
    unsigned Task;
    StringRef ModuleID;
    StringRef NativeObjectPath;
    StringRef SummaryIndexPath;
    ImportsFilesContainer ImportsFiles;
  };
  // The set of backend compilations jobs.
  SmallVector<Job> Jobs;

  // A unique string to identify the current link.
  SmallString<8> UID;

  // The offset to the first ThinLTO task.
  unsigned ThinLTOTaskOffset;

  // The target triple to supply for backend compilations.
  llvm::Triple Triple;

public:
  OutOfProcessThinBackend(
      const Config &Conf, ModuleSummaryIndex &CombinedIndex,
      ThreadPoolStrategy ThinLTOParallelism,
      const DenseMap<StringRef, GVSummaryMapTy> &ModuleToDefinedGVSummaries,
      AddStreamFn AddStream, lto::IndexWriteCallback OnWrite,
      bool ShouldEmitIndexFiles, bool ShouldEmitImportsFiles,
      StringRef LinkerOutputFile, StringRef Distributor,
      ArrayRef<StringRef> DistributorArgs, StringRef RemoteCompiler,
      ArrayRef<StringRef> RemoteCompilerArgs, bool SaveTemps)
      : CGThinBackend(Conf, CombinedIndex, ModuleToDefinedGVSummaries,
                      AddStream, OnWrite, ShouldEmitIndexFiles,
                      ShouldEmitImportsFiles, ThinLTOParallelism),
        LinkerOutputFile(LinkerOutputFile), DistributorPath(Distributor),
        DistributorArgs(DistributorArgs), RemoteCompiler(RemoteCompiler),
        RemoteCompilerArgs(RemoteCompilerArgs), SaveTemps(SaveTemps) {}

  virtual void setup(unsigned ThinLTONumTasks, unsigned ThinLTOTaskOffset,
                     llvm::Triple Triple) override {
    UID = itostr(sys::Process::getProcessId());
    Jobs.resize((size_t)ThinLTONumTasks);
    this->ThinLTOTaskOffset = ThinLTOTaskOffset;
    this->Triple = Triple;
  }

  Error start(
      unsigned Task, BitcodeModule BM,
      const FunctionImporter::ImportMapTy &ImportList,
      const FunctionImporter::ExportSetTy &ExportList,
      const std::map<GlobalValue::GUID, GlobalValue::LinkageTypes> &ResolvedODR,
      MapVector<StringRef, BitcodeModule> &ModuleMap) override {

    StringRef ModulePath = BM.getModuleIdentifier();

    SString ObjFilePath = sys::path::parent_path(LinkerOutputFile);
    sys::path::append(ObjFilePath, sys::path::stem(ModulePath) + "." +
                                       itostr(Task) + "." + UID + ".native.o");

    Job &J = Jobs[Task - ThinLTOTaskOffset];
    J = {
        Task,
        ModulePath,
        Saver.save(ObjFilePath.str()),
        Saver.save(ObjFilePath.str() + ".thinlto.bc"),
        {} // Filled in by emitFiles below.
    };

    assert(ModuleToDefinedGVSummaries.count(ModulePath));

    // The BackendThreadPool is only used here to write the sharded index files
    // (similar to WriteIndexesThinBackend).
    BackendThreadPool.async(
        [=](Job &J, const FunctionImporter::ImportMapTy &ImportList) {
          if (auto E = emitFiles(ImportList, J.ModuleID, J.ModuleID.str(),
                                 J.SummaryIndexPath, J.ImportsFiles)) {
            std::unique_lock<std::mutex> L(ErrMu);
            if (Err)
              Err = joinErrors(std::move(*Err), std::move(E));
            else
              Err = std::move(E);
          }
        },
        std::ref(J), std::ref(ImportList));

    return Error::success();
  }

  // Derive a set of Clang options that will be shared/common for all DTLTO
  // backend compilations. We are intentionally minimal here as these options
  // must remain synchronized with the behavior of Clang. DTLTO does not support
  // all the features available with in-process LTO. More features are expected
  // to be added over time. Users can specify Clang options directly if a
  // feature is not supported. Note that explicitly specified options that imply
  // additional input or output file dependencies must be communicated to the
  // distribution system, potentially by setting extra options on the
  // distributor program.
  void buildCommonRemoteCompilerOptions() {
    const lto::Config &C = Conf;
    auto &Ops = CodegenOptions;

    Ops.push_back(Saver.save("-O" + Twine(C.OptLevel)));

    if (C.Options.EmitAddrsig)
      Ops.push_back("-faddrsig");
    if (C.Options.FunctionSections)
      Ops.push_back("-ffunction-sections");
    if (C.Options.DataSections)
      Ops.push_back("-fdata-sections");

    if (C.RelocModel == Reloc::PIC_)
      // Clang doesn't have -fpic for all triples.
      if (!Triple.isOSBinFormatCOFF())
        Ops.push_back("-fpic");

    // Turn on/off warnings about profile cfg mismatch (default on)
    // --lto-pgo-warn-mismatch.
    if (!C.PGOWarnMismatch) {
      Ops.push_back("-mllvm");
      Ops.push_back("-no-pgo-warn-mismatch");
    }

    // Enable sample-based profile guided optimizations.
    // Sample profile file path --lto-sample-profile=<value>.
    if (!C.SampleProfile.empty()) {
      Ops.push_back(
          Saver.save("-fprofile-sample-use=" + Twine(C.SampleProfile)));
      CommonInputs.insert(C.SampleProfile);
    }

    // We don't know which of options will be used by Clang.
    Ops.push_back("-Wno-unused-command-line-argument");

    // Forward any supplied options.
    if (!RemoteCompilerArgs.empty())
      for (auto &a : RemoteCompilerArgs)
        Ops.push_back(a);
  }

  // Generates a JSON file describing the backend compilations, for the
  // distributor.
  bool emitDistributorJson(StringRef DistributorJson) {
    using json::Array;
    std::error_code EC;
    raw_fd_ostream OS(DistributorJson, EC);
    if (EC)
      return false;

    json::OStream JOS(OS);
    JOS.object([&]() {
      // Information common to all jobs.
      JOS.attributeObject("common", [&]() {
        JOS.attribute("linker_output", LinkerOutputFile);

        JOS.attributeArray("args", [&]() {
          JOS.value(RemoteCompiler);

          JOS.value("-c");

          JOS.value(Saver.save("--target=" + Triple.str()));

          for (const auto &A : CodegenOptions)
            JOS.value(A);
        });

        JOS.attribute("inputs", Array(CommonInputs));
      });

      // Per-compilation-job information.
      JOS.attributeArray("jobs", [&]() {
        for (const auto &J : Jobs) {
          assert(J.Task != 0);

          SmallVector<StringRef, 2> Inputs;
          SmallVector<StringRef, 1> Outputs;

          JOS.object([&]() {
            JOS.attributeArray("args", [&]() {
              JOS.value(J.ModuleID);
              Inputs.push_back(J.ModuleID);

              JOS.value(
                  Saver.save("-fthinlto-index=" + Twine(J.SummaryIndexPath)));
              Inputs.push_back(J.SummaryIndexPath);

              JOS.value("-o");
              JOS.value(J.NativeObjectPath);
              Outputs.push_back(J.NativeObjectPath);
            });

            // Add the bitcode files from which imports will be made. These do
            // not explicitly appear on the backend compilation command lines
            // but are recorded in the summary index shards.
            llvm::append_range(Inputs, J.ImportsFiles);
            JOS.attribute("inputs", Array(Inputs));

            JOS.attribute("outputs", Array(Outputs));
          });
        }
      });
    });

    return true;
  }

  void removeFile(StringRef FileName) {
    std::error_code EC = sys::fs::remove(FileName, true);
    if (EC && EC != std::make_error_code(std::errc::no_such_file_or_directory))
      errs() << "warning: could not remove the file '" << FileName
             << "': " << EC.message() << "\n";
  }

  Error wait() override {
    // Wait for the information on the required backend compilations to be
    // gathered.
    BackendThreadPool.wait();
    if (Err)
      return std::move(*Err);

    auto CleanPerJobFiles = llvm::make_scope_exit([&] {
      if (!SaveTemps)
        for (auto &Job : Jobs) {
          removeFile(Job.NativeObjectPath);
          if (!ShouldEmitIndexFiles)
            removeFile(Job.SummaryIndexPath);
        }
    });

    const StringRef BCError = "DTLTO backend compilation: ";

    buildCommonRemoteCompilerOptions();

    SString JsonFile = sys::path::parent_path(LinkerOutputFile);
    sys::path::append(JsonFile, sys::path::stem(LinkerOutputFile) + "." + UID +
                                    ".dist-file.json");
    if (!emitDistributorJson(JsonFile))
      return make_error<StringError>(
          BCError + "failed to generate distributor JSON script: " + JsonFile,
          inconvertibleErrorCode());
    auto CleanJson = llvm::make_scope_exit([&] {
      if (!SaveTemps)
        removeFile(JsonFile);
    });

    SmallVector<StringRef, 3> Args = {DistributorPath};
    llvm::append_range(Args, DistributorArgs);
    Args.push_back(JsonFile);
    std::string ErrMsg;
    if (sys::ExecuteAndWait(Args[0], Args,
                            /*Env=*/std::nullopt, /*Redirects=*/{},
                            /*SecondsToWait=*/0, /*MemoryLimit=*/0, &ErrMsg)) {
      return make_error<StringError>(
          BCError + "distributor execution failed" +
              (!ErrMsg.empty() ? ": " + ErrMsg + Twine(".") : Twine(".")),
          inconvertibleErrorCode());
    }

    for (auto &Job : Jobs) {
      // Load the native object from a file into a memory buffer
      // and store its contents in the output buffer.
      auto ObjFileMbOrErr =
          MemoryBuffer::getFile(Job.NativeObjectPath, /*IsText=*/false,
                                /*RequiresNullTerminator=*/false);
      if (std::error_code EC = ObjFileMbOrErr.getError())
        return make_error<StringError>(
            BCError + "cannot open native object file: " +
                Job.NativeObjectPath + ": " + EC.message(),
            inconvertibleErrorCode());
      auto StreamOrErr = AddStream(Job.Task, Job.ModuleID);
      if (Error Err = StreamOrErr.takeError())
        report_fatal_error(std::move(Err));
      auto &Stream = *StreamOrErr->get();
      *Stream.OS << ObjFileMbOrErr->get()->getMemBufferRef().getBuffer();
      if (Error Err = Stream.commit())
        report_fatal_error(std::move(Err));
    }

    return Error::success();
  }
};
} // end anonymous namespace

ThinBackend lto::createOutOfProcessThinBackend(
    ThreadPoolStrategy Parallelism, lto::IndexWriteCallback OnWrite,
    bool ShouldEmitIndexFiles, bool ShouldEmitImportsFiles,
    StringRef LinkerOutputFile, StringRef Distributor,
    ArrayRef<StringRef> DistributorArgs, StringRef RemoteCompiler,
    ArrayRef<StringRef> RemoteCompilerArgs, bool SaveTemps) {
  auto Func =
      [=](const Config &Conf, ModuleSummaryIndex &CombinedIndex,
          const DenseMap<StringRef, GVSummaryMapTy> &ModuleToDefinedGVSummaries,
          AddStreamFn AddStream, FileCache /*Cache*/) {
        return std::make_unique<OutOfProcessThinBackend>(
            Conf, CombinedIndex, Parallelism, ModuleToDefinedGVSummaries,
            AddStream, OnWrite, ShouldEmitIndexFiles, ShouldEmitImportsFiles,
            LinkerOutputFile, Distributor, DistributorArgs, RemoteCompiler,
            RemoteCompilerArgs, SaveTemps);
      };
  return ThinBackend(Func, Parallelism);
}
