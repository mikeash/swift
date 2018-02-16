//===--- ToolChain.cpp - Collections of tools for one platform ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
/// \file This file defines the base implementation of the ToolChain class.
/// The platform-specific subclasses are implemented in ToolChains.cpp.
/// For organizational purposes, the platform-independent logic for
/// constructing job invocations is also located in ToolChains.cpp.
//
//===----------------------------------------------------------------------===//

#include "swift/Driver/ToolChain.h"
#include "swift/Driver/Compilation.h"
#include "swift/Driver/Driver.h"
#include "swift/Driver/Job.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/ADT/STLExtras.h"

using namespace swift;
using namespace swift::driver;
using namespace llvm::opt;

ToolChain::JobContext::JobContext(Compilation &C,
                                  ArrayRef<const Job *> Inputs,
                                  ArrayRef<const Action *> InputActions,
                                  const CommandOutput &Output,
                                  const OutputInfo &OI)
  : C(C), Inputs(Inputs), InputActions(InputActions), Output(Output),
    OI(OI), Args(C.getArgs()) {}

ArrayRef<InputPair> ToolChain::JobContext::getTopLevelInputFiles() const {
  return C.getInputFiles();
}
const char *ToolChain::JobContext::getAllSourcesPath() const {
  return C.getAllSourcesPath();
}

const char *
ToolChain::JobContext::getTemporaryFilePath(const llvm::Twine &name,
                                            StringRef suffix) const {
  SmallString<128> buffer;
  std::error_code EC =
      llvm::sys::fs::createTemporaryFile(name, suffix, buffer);
  if (EC) {
    // FIXME: This should not take down the entire process.
    llvm::report_fatal_error("unable to create temporary file for filelist");
  }

  C.addTemporaryFile(buffer.str(), PreserveOnSignal::Yes);
  // We can't just reference the data in the TemporaryFiles vector because
  // that could theoretically get copied to a new address.
  return C.getArgs().MakeArgString(buffer.str());
}

std::unique_ptr<Job>
ToolChain::constructJob(const JobAction &JA,
                        Compilation &C,
                        SmallVectorImpl<const Job *> &&inputs,
                        ArrayRef<const Action *> inputActions,
                        std::unique_ptr<CommandOutput> output,
                        const OutputInfo &OI) const {
  JobContext context{C, inputs, inputActions, *output, OI};

  auto invocationInfo = [&]() -> InvocationInfo {
    switch (JA.getKind()) {
  #define CASE(K) case Action::K: \
      return constructInvocation(cast<K##Action>(JA), context);
    CASE(CompileJob)
    CASE(InterpretJob)
    CASE(BackendJob)
    CASE(MergeModuleJob)
    CASE(ModuleWrapJob)
    CASE(LinkJob)
    CASE(GenerateDSYMJob)
    CASE(VerifyDebugInfoJob)
    CASE(GeneratePCHJob)
    CASE(AutolinkExtractJob)
    CASE(REPLJob)
#undef CASE
    case Action::Input:
      llvm_unreachable("not a JobAction");
    }

    // Work around MSVC warning: not all control paths return a value
    llvm_unreachable("All switch cases are covered");
  }();

  // Special-case the Swift frontend.
  const char *executablePath = nullptr;
  if (StringRef(SWIFT_EXECUTABLE_NAME) == invocationInfo.ExecutableName) {
    executablePath = getDriver().getSwiftProgramPath().c_str();
  } else {
    std::string relativePath =
        findProgramRelativeToSwift(invocationInfo.ExecutableName);
    if (!relativePath.empty()) {
      executablePath = C.getArgs().MakeArgString(relativePath);
    } else {
      auto systemPath =
          llvm::sys::findProgramByName(invocationInfo.ExecutableName);
      if (systemPath) {
        executablePath = C.getArgs().MakeArgString(systemPath.get());
      } else {
        // For debugging purposes.
        executablePath = invocationInfo.ExecutableName;
      }
    }
  }

  return llvm::make_unique<Job>(JA, std::move(inputs), std::move(output),
                                executablePath,
                                std::move(invocationInfo.Arguments),
                                std::move(invocationInfo.ExtraEnvironment),
                                std::move(invocationInfo.FilelistInfos));
}

std::string
ToolChain::findProgramRelativeToSwift(StringRef executableName) const {
  auto insertionResult =
      ProgramLookupCache.insert(std::make_pair(executableName, ""));
  if (insertionResult.second) {
    std::string path = findProgramRelativeToSwiftImpl(executableName);
    insertionResult.first->setValue(std::move(path));
  }
  return insertionResult.first->getValue();
}

std::string
ToolChain::findProgramRelativeToSwiftImpl(StringRef executableName) const {
  StringRef swiftPath = getDriver().getSwiftProgramPath();
  StringRef swiftBinDir = llvm::sys::path::parent_path(swiftPath);

  auto result = llvm::sys::findProgramByName(executableName, {swiftBinDir});
  if (result)
    return result.get();
  return {};
}

types::ID ToolChain::lookupTypeForExtension(StringRef Ext) const {
  return types::lookupTypeForExtension(Ext);
}

/// Return a _single_ TY_Swift InputAction, if one exists;
/// if 0 or >1 such inputs exist, return nullptr.
static const InputAction*
findSingleSwiftInput(const CompileJobAction *CJA) {
  auto Inputs = CJA->getInputs();
  const InputAction *IA = nullptr;
  for (auto const *I : Inputs) {
    if (auto const *S = dyn_cast<InputAction>(I)) {
      if (S->getType() == types::TY_Swift) {
        if (IA == nullptr) {
          IA = S;
        } else {
          // Already found one, two is too many.
          return nullptr;
        }
      }
    }
  }
  return IA;
}

static bool
jobsHaveSameExecutableNames(const Job *A, const Job *B) {
  // Jobs that get here (that are derived from CompileJobActions) should always
  // have the same executable name -- it should always be SWIFT_EXECUTABLE_NAME
  // -- but we check here just to be sure / fail gracefully in non-assert
  // builds.
  assert(strcmp(A->getExecutable(), B->getExecutable()) == 0);
  if (strcmp(A->getExecutable(), B->getExecutable()) != 0) {
    return false;
  }
  return true;
}

static bool
jobsHaveSameOutputTypes(const Job *A, const Job *B) {
  if (A->getOutput().getPrimaryOutputType() !=
      B->getOutput().getPrimaryOutputType())
    return false;
  return A->getOutput().hasSameAdditionalOutputTypes(B->getOutput());
}

static bool
jobsHaveSameEnvironment(const Job *A, const Job *B) {
  auto AEnv = A->getExtraEnvironment();
  auto BEnv = B->getExtraEnvironment();
  if (AEnv.size() != BEnv.size())
    return false;
  for (size_t i = 0; i < AEnv.size(); ++i) {
    if (strcmp(AEnv[i].first, BEnv[i].first) != 0)
      return false;
    if (strcmp(AEnv[i].second, BEnv[i].second) != 0)
      return false;
  }
  return true;
}

bool
ToolChain::jobIsBatchable(const Compilation &C, const Job *A) const {
  // FIXME: There might be a tighter criterion to use here?
  if (C.getOutputInfo().CompilerMode != OutputInfo::Mode::StandardCompile)
    return false;
  auto const *CJActA = dyn_cast<const CompileJobAction>(&A->getSource());
  if (!CJActA)
    return false;
  return findSingleSwiftInput(CJActA) != nullptr;
}

bool
ToolChain::jobsAreBatchCombinable(const Compilation &C,
                                  const Job *A, const Job *B) const {
  assert(jobIsBatchable(C, A));
  assert(jobIsBatchable(C, B));
  return (jobsHaveSameExecutableNames(A, B) &&
          jobsHaveSameOutputTypes(A, B) &&
          jobsHaveSameEnvironment(A, B));
}

/// Form a synthetic \c CommandOutput for a \c BatchJob by merging together the
/// \c CommandOutputs of all the jobs passed.
static std::unique_ptr<CommandOutput>
makeBatchCommandOutput(ArrayRef<const Job *> jobs, Compilation &C,
                       types::ID outputType) {
  auto output =
      llvm::make_unique<CommandOutput>(outputType, C.getDerivedOutputFileMap());
  for (auto const *J : jobs) {
    output->addOutputs(J->getOutput());
  }
  return output;
}

/// Set-union the \c Inputs and \c Actions from each \c Job in \p jobs into the
/// provided \p inputJobs and \p inputActions vectors, further adding all \c
/// Actions from the resulting merger to \p batchCJA. Do set-union rather than
/// concatenation here to avoid mentioning the same input multiple times.
static bool
mergeBatchInputs(ArrayRef<const Job *> jobs,
                 llvm::SmallSetVector<const Job *, 16> &inputJobs,
                 llvm::SmallSetVector<const Action *, 16> &inputActions,
                 CompileJobAction *batchCJA) {
  for (auto const *J : jobs) {
    for (auto const *I : J->getInputs()) {
      inputJobs.insert(I);
    }
    auto const *CJA = dyn_cast<CompileJobAction>(&J->getSource());
    if (!CJA)
      return true;
    for (auto const *I : CJA->getInputs()) {
      inputActions.insert(I);
    }
  }

  for (auto const *I : inputActions) {
    batchCJA->addInput(I);
  }
  return false;
}

/// Construct a \c BatchJob by merging the constituent \p jobs' CommandOutput,
/// input \c Job and \c Action members. Call through to \c constructInvocation
/// on \p BatchJob, to build the \c InvocationInfo.
std::unique_ptr<Job>
ToolChain::constructBatchJob(ArrayRef<const Job *> jobs,
                             Compilation &C) const
{
#ifndef NDEBUG
  // Verify that the equivalence relation on the jobs also holds pairwise.
  for (auto *A : jobs) {
    for (auto *B : jobs) {
      assert(jobsAreBatchCombinable(C, A, B));
    }
  }
#endif
  if (jobs.size() == 0)
    return nullptr;

  // Synthetic OutputInfo is a slightly-modified version of the initial
  // compilation's OI.
  auto OI = C.getOutputInfo();
  OI.CompilerMode = OutputInfo::Mode::BatchModeCompile;

  auto const *executablePath = jobs[0]->getExecutable();
  auto outputType = jobs[0]->getOutput().getPrimaryOutputType();
  auto output = makeBatchCommandOutput(jobs, C, outputType);

  llvm::SmallSetVector<const Job *, 16> inputJobs;
  llvm::SmallSetVector<const Action *, 16> inputActions;
  auto *batchCJA = C.createAction<CompileJobAction>(outputType);
  if (mergeBatchInputs(jobs, inputJobs, inputActions, batchCJA))
    return nullptr;

  JobContext context{C, inputJobs.getArrayRef(), inputActions.getArrayRef(),
                     *output, OI};
  auto invocationInfo = constructInvocation(*batchCJA, context);
  return llvm::make_unique<BatchJob>(*batchCJA,
                                     inputJobs.takeVector(),
                                     std::move(output),
                                     executablePath,
                                     std::move(invocationInfo.Arguments),
                                     std::move(invocationInfo.ExtraEnvironment),
                                     std::move(invocationInfo.FilelistInfos),
                                     jobs);
}

bool
ToolChain::sanitizerRuntimeLibExists(const ArgList &args,
                                     StringRef sanitizerName) const {
  // Assume no sanitizers are supported by default.
  // This method should be overriden by a platform-specific subclass.
  return false;
}
